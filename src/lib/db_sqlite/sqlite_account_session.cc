#include <chrono>
#include <format>

#include "db/account_session.h"
#include "db_sqlite/sqlite_database.h"

namespace atlas {

auto SqliteDatabase::ReadAccountSessionImpl(DatabaseID dbid, uint16_t type_id)
    -> Result<AccountSession> {
  if (!started_ || db_ == nullptr)
    return Error{ErrorCode::kIoError, "account session database unavailable"};
  if (dbid <= kInvalidDBID || type_id == 0)
    return Error{ErrorCode::kInvalidArgument, "invalid account session key"};
  auto statement =
      Prepare("SELECT version, payload FROM atlas_account_sessions WHERE dbid = ? AND type_id = ?");
  if (!statement) return statement.Error();
  auto* stmt = statement->Get();
  if (sqlite3_bind_int64(stmt, 1, dbid) != SQLITE_OK ||
      sqlite3_bind_int(stmt, 2, type_id) != SQLITE_OK)
    return SqliteError("account session key bind failed");
  const auto rc = sqlite3_step(stmt);
  if (rc == SQLITE_DONE) return AccountSession{};
  if (rc != SQLITE_ROW) return SqliteError("account session read failed", rc);
  const auto version = sqlite3_column_int64(stmt, 0);
  const auto size = sqlite3_column_bytes(stmt, 1);
  const auto* data = static_cast<const std::byte*>(sqlite3_column_blob(stmt, 1));
  if (sqlite3_column_type(stmt, 0) != SQLITE_INTEGER || version <= 0 ||
      sqlite3_column_type(stmt, 1) != SQLITE_BLOB || data == nullptr || size <= 0 || size > 256)
    return Error{ErrorCode::kIoError, "invalid account session row"};
  BinaryReader reader(std::span{data, static_cast<std::size_t>(size)});
  auto session = atlas::ReadAccountSession(reader);
  if (!session || reader.Remaining() != 0 || session->version != static_cast<uint64_t>(version))
    return Error{ErrorCode::kIoError, "invalid account session payload"};
  return *session;
}

void SqliteDatabase::ReadAccountSession(DatabaseID dbid, uint16_t type_id,
                                        std::function<void(Result<AccountSession>)> callback) {
  if (!AdmitCallback(callback)) return;
  FireOrDefer(std::move(callback), ReadAccountSessionImpl(dbid, type_id));
}

void SqliteDatabase::UpdateAccountSession(const AccountSessionOperation& op,
                                          std::function<void(Result<AccountSession>)> callback) {
  if (!AdmitCallback(callback)) return;
  if (auto valid = ValidateSessionOperation(op); !valid) {
    FireOrDefer(std::move(callback), Result<AccountSession>{valid.Error()});
    return;
  }
  if (!started_ || db_ == nullptr) {
    FireOrDefer(std::move(callback),
                Result<AccountSession>{Error{ErrorCode::kIoError, "session database unavailable"}});
    return;
  }
  auto hash = HashSessionOperation(op);
  if (!hash) {
    FireOrDefer(std::move(callback), Result<AccountSession>{hash.Error()});
    return;
  }
  auto scope = BeginWriteScope();
  if (!scope) {
    FireOrDefer(std::move(callback), Result<AccountSession>{scope.Error()});
    return;
  }
  bool modified = false;
  auto execute = [&]() -> Result<AccountSession> {
    auto previous = FindOperationReceipt(op.id);
    if (!previous) return previous.Error();
    const CheckoutInfo actor{op.actor.base_addr, 0, op.actor.entity_id, 0,
                             op.actor.process_instance};
    if (auto active = CheckCheckoutProcess(actor); !active) return active.Error();
    if (*previous) {
      if ((**previous).request_hash != *hash)
        return Error{ErrorCode::kAlreadyExists, "operation_id_reused"};
      auto current = ReadAccountSessionImpl(op.account_dbid, op.account_type_id);
      if (!current) return current.Error();
      return ConfirmSessionReceipt(op, **previous, *current);
    }
    if (op.action == AccountSessionAction::kClaim ||
        op.action == AccountSessionAction::kClaimRecovery) {
      auto account = FetchByDbid(op.account_dbid, op.account_type_id);
      if (!account) return account.Error();
      if (!account->found) return Error{ErrorCode::kNotFound, "session account does not exist"};
      if (account->checked_out_by != op.claim_checkout)
        return Error{ErrorCode::kPermissionDenied, "session account checkout changed"};
    }
    if (op.action == AccountSessionAction::kPrepare ||
        op.action == AccountSessionAction::kMoveLocal) {
      const CheckoutInfo target{op.target.base_addr, 0, op.target.entity_id, 0,
                                op.target.process_instance};
      if (auto active = CheckCheckoutProcess(target); !active) return active.Error();
    }
    auto current = ReadAccountSessionImpl(op.account_dbid, op.account_type_id);
    if (!current) return current.Error();
    if (op.action == AccountSessionAction::kClaimRecovery) {
      const auto& old = current->owner;
      auto retired = Prepare(std::format(
          "SELECT retired FROM atlas_checkout_processes WHERE base_ip = {} AND base_port = {} "
          "AND instance = {}",
          old.base_addr.Ip(), old.base_addr.Port(), old.process_instance));
      if (!retired) return retired.Error();
      const auto rc = sqlite3_step(retired->Get());
      if (rc != SQLITE_ROW && rc != SQLITE_DONE)
        return SqliteError("retired session owner lookup failed", rc);
      if (rc != SQLITE_ROW || sqlite3_column_type(retired->Get(), 0) != SQLITE_INTEGER ||
          sqlite3_column_int(retired->Get(), 0) != 1)
        return Error{ErrorCode::kPermissionDenied, "previous session owner retirement unconfirmed"};
      auto entity = FetchByDbid(current->recovery.entity_dbid, current->recovery.entity_type_id);
      if (!entity) return entity.Error();
      if (!entity->found || entity->checked_out_by != op.recovery_checkout)
        return Error{ErrorCode::kPermissionDenied, "recovery target checkout changed"};
    }
    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
    auto next = ApplySessionOperation(*current, op, now_ms);
    if (!next) return next.Error();
    BinaryWriter payload;
    WriteAccountSession(payload, *next);
    auto statement = Prepare(
        "INSERT INTO atlas_account_sessions(dbid, type_id, version, payload) VALUES(?, ?, ?, ?) "
        "ON CONFLICT(dbid, type_id) DO UPDATE SET version = excluded.version, "
        "payload = excluded.payload");
    if (!statement) return statement.Error();
    auto* stmt = statement->Get();
    if (sqlite3_bind_int64(stmt, 1, op.account_dbid) != SQLITE_OK ||
        sqlite3_bind_int(stmt, 2, op.account_type_id) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 3, static_cast<int64_t>(next->version)) != SQLITE_OK ||
        sqlite3_bind_blob(stmt, 4, payload.Data().data(), static_cast<int>(payload.Data().size()),
                          SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_step(stmt) != SQLITE_DONE)
      return SqliteError("account session write failed");
    auto saved = SaveOperationReceipt(
        op.id, DatabaseOperationReceipt{*hash, op.account_dbid, next->version});
    if (!saved) return saved.Error();
    modified = true;
    return *next;
  };
  auto result = execute();
  if (!result || !modified) {
    RollbackWriteScope(*scope);
  } else if (auto committed = CommitWriteScope(*scope); !committed) {
    result = committed.Error();
  }
  FireOrDefer(std::move(callback), std::move(result));
}

}  // namespace atlas
