#include <mysql.h>

#include <charconv>
#include <format>
#include <memory>

#include "db/account_session.h"
#include "db_mysql/mysql_database.h"

namespace atlas {

auto MysqlDatabase::DoReadAccountSession(MYSQL* conn, DatabaseID dbid, uint16_t type_id,
                                         bool for_update) -> Result<AccountSession> {
  if (conn == nullptr) return Error{ErrorCode::kIoError, "account session database unavailable"};
  if (dbid <= kInvalidDBID || type_id == 0)
    return Error{ErrorCode::kInvalidArgument, "invalid account session key"};
  const auto sql = std::format(
      "SELECT version, payload FROM atlas_account_sessions WHERE dbid = {} AND type_id = {}{}",
      dbid, type_id, for_update ? " FOR UPDATE" : "");
  if (mysql_real_query(conn, sql.data(), static_cast<unsigned long>(sql.size())) != 0)
    return MysqlErr(conn, "account session read failed");
  std::unique_ptr<MYSQL_RES, decltype(&mysql_free_result)> rows(mysql_store_result(conn),
                                                                mysql_free_result);
  if (!rows) return MysqlErr(conn, "account session result unavailable");
  auto row = mysql_fetch_row(rows.get());
  if (row == nullptr) return AccountSession{};
  auto* lengths = mysql_fetch_lengths(rows.get());
  if (row[0] == nullptr || row[1] == nullptr || lengths == nullptr || lengths[1] > 256)
    return Error{ErrorCode::kIoError, "invalid account session row"};
  uint64_t version = 0;
  const auto parsed = std::from_chars(row[0], row[0] + lengths[0], version);
  if (parsed.ec != std::errc{} || parsed.ptr != row[0] + lengths[0] || version == 0)
    return Error{ErrorCode::kIoError, "invalid account session row version"};
  BinaryReader reader(std::as_bytes(std::span{row[1], static_cast<std::size_t>(lengths[1])}));
  auto session = atlas::ReadAccountSession(reader);
  if (!session || reader.Remaining() != 0 || session->version != version)
    return Error{ErrorCode::kIoError, "invalid account session payload"};
  return *session;
}

auto MysqlDatabase::DoUpdateAccountSession(MYSQL* conn, const AccountSessionOperation& op)
    -> Result<AccountSession> {
  if (auto valid = ValidateSessionOperation(op); !valid) return valid.Error();
  auto hash = HashSessionOperation(op);
  if (!hash) return hash.Error();
  if (auto begun = BeginTxn(conn); !begun) return begun.Error();
  bool modified = false;
  auto execute = [&]() -> Result<AccountSession> {
    auto previous = LockOperationReceipt(conn, op.id, *hash);
    if (!previous) return previous.Error();
    const CheckoutInfo actor{op.actor.base_addr, 0, op.actor.entity_id, 0,
                             op.actor.process_instance};
    if (auto active = CheckCheckoutProcess(conn, actor); !active) return active.Error();
    if (*previous) {
      auto current = DoReadAccountSession(conn, op.account_dbid, op.account_type_id, true);
      if (!current) return current.Error();
      return ConfirmSessionReceipt(op, **previous, *current);
    }
    if (op.action == AccountSessionAction::kClaim ||
        op.action == AccountSessionAction::kClaimRecovery) {
      auto account = FetchByDbid(conn, op.account_dbid, op.account_type_id, true);
      if (!account) return account.Error();
      if (!account->found) return Error{ErrorCode::kNotFound, "session account does not exist"};
      if (account->checked_out_by != op.claim_checkout)
        return Error{ErrorCode::kPermissionDenied, "session account checkout changed"};
    }
    if (op.action == AccountSessionAction::kPrepare ||
        op.action == AccountSessionAction::kMoveLocal) {
      const CheckoutInfo target{op.target.base_addr, 0, op.target.entity_id, 0,
                                op.target.process_instance};
      if (auto active = CheckCheckoutProcess(conn, target); !active) return active.Error();
    }
    auto current = DoReadAccountSession(conn, op.account_dbid, op.account_type_id, true);
    if (!current) return current.Error();
    if (op.action == AccountSessionAction::kClaimRecovery) {
      const auto& old = current->owner;
      auto retired = QueryScalar(
          conn,
          std::format(
              "SELECT retired FROM atlas_checkout_processes WHERE base_ip = {} AND base_port = {} "
              "AND instance = {} FOR UPDATE",
              old.base_addr.Ip(), old.base_addr.Port(), old.process_instance));
      if (!retired) return retired.Error();
      if (!*retired || **retired != "1")
        return Error{ErrorCode::kPermissionDenied, "previous session owner retirement unconfirmed"};
      auto entity =
          FetchByDbid(conn, current->recovery.entity_dbid, current->recovery.entity_type_id, true);
      if (!entity) return entity.Error();
      if (!entity->found || entity->checked_out_by != op.recovery_checkout)
        return Error{ErrorCode::kPermissionDenied, "recovery target checkout changed"};
    }
    int64_t now_ms = 0;
    if (op.action == AccountSessionAction::kIssueRecovery ||
        op.action == AccountSessionAction::kConsumeRecovery ||
        op.action == AccountSessionAction::kClaimRecovery) {
      auto time =
          QueryScalar(conn, "SELECT CAST(UNIX_TIMESTAMP(CURRENT_TIMESTAMP(3)) * 1000 AS SIGNED)");
      if (!time) return time.Error();
      if (!*time) return Error{ErrorCode::kIoError, "database time unavailable"};
      const auto& value = **time;
      const auto parsed = std::from_chars(value.data(), value.data() + value.size(), now_ms);
      if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
        return Error{ErrorCode::kIoError, "invalid database time"};
    }
    auto next = ApplySessionOperation(*current, op, now_ms);
    if (!next) return next.Error();
    BinaryWriter payload;
    WriteAccountSession(payload, *next);
    auto saved = ExecSql(
        conn, std::format("INSERT INTO atlas_account_sessions(dbid, type_id, version, payload) "
                          "VALUES({}, {}, {}, {}) ON DUPLICATE KEY UPDATE "
                          "version = VALUES(version), payload = VALUES(payload)",
                          op.account_dbid, op.account_type_id, next->version,
                          HexLiteral(payload.Data())));
    if (!saved) return saved.Error();
    auto receipted = SaveOperationReceipt(
        conn, op.id, DatabaseOperationReceipt{*hash, op.account_dbid, next->version});
    if (!receipted) return receipted.Error();
    modified = true;
    return *next;
  };
  auto result = execute();
  if (!result || !modified) {
    Rollback(conn);
    return result;
  }
  if (auto committed = Commit(conn); !committed) {
    Rollback(conn);
    return committed.Error();
  }
  return result;
}

void MysqlDatabase::ReadAccountSession(DatabaseID dbid, uint16_t type_id,
                                       std::function<void(Result<AccountSession>)> callback) {
  Dispatch<Result<AccountSession>>(
      type_id, static_cast<uint64_t>(dbid),
      [dbid, type_id](MYSQL* conn) { return DoReadAccountSession(conn, dbid, type_id); },
      std::move(callback));
}

void MysqlDatabase::UpdateAccountSession(const AccountSessionOperation& operation,
                                         std::function<void(Result<AccountSession>)> callback) {
  Dispatch<Result<AccountSession>>(
      operation.account_type_id, static_cast<uint64_t>(operation.account_dbid),
      [operation](MYSQL* conn) { return DoUpdateAccountSession(conn, operation); },
      std::move(callback));
}

}  // namespace atlas
