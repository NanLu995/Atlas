#include <cstring>

#include "db_sqlite/sqlite_database.h"

namespace atlas {

auto SqliteDatabase::FindOperationReceipt(const std::array<std::byte, 16>& id)
    -> Result<std::optional<DatabaseOperationReceipt>> {
  auto statement = Prepare(
      "SELECT request_hash, result_dbid, checkout_version "
      "FROM atlas_write_receipts WHERE operation_id = ?");
  if (!statement) return statement.Error();
  auto* stmt = statement->Get();
  if (sqlite3_bind_blob(stmt, 1, id.data(), static_cast<int>(id.size()), SQLITE_TRANSIENT) !=
      SQLITE_OK)
    return SqliteError("receipt key bind failed");
  const auto rc = sqlite3_step(stmt);
  if (rc == SQLITE_DONE) return std::optional<DatabaseOperationReceipt>{};
  if (rc != SQLITE_ROW) return SqliteError("receipt read failed", rc);
  DatabaseOperationReceipt receipt;
  const auto size = sqlite3_column_bytes(stmt, 0);
  const auto* data = sqlite3_column_blob(stmt, 0);
  if (sqlite3_column_type(stmt, 0) != SQLITE_BLOB || data == nullptr ||
      size != static_cast<int>(receipt.request_hash.size()) ||
      sqlite3_column_type(stmt, 1) != SQLITE_INTEGER || sqlite3_column_int64(stmt, 1) <= 0 ||
      sqlite3_column_type(stmt, 2) != SQLITE_INTEGER || sqlite3_column_int64(stmt, 2) < 0) {
    return Error{ErrorCode::kIoError, "invalid SQLite write receipt"};
  }
  std::memcpy(receipt.request_hash.data(), data, receipt.request_hash.size());
  receipt.dbid = sqlite3_column_int64(stmt, 1);
  receipt.checkout_version = static_cast<uint64_t>(sqlite3_column_int64(stmt, 2));
  return std::optional{receipt};
}

auto SqliteDatabase::SaveOperationReceipt(const std::array<std::byte, 16>& id,
                                          const DatabaseOperationReceipt& receipt) -> Result<void> {
  auto statement = Prepare(
      "INSERT INTO atlas_write_receipts "
      "(operation_id, request_hash, result_dbid, checkout_version) "
      "VALUES(?, ?, ?, ?)");
  if (!statement) return statement.Error();
  auto* stmt = statement->Get();
  if (sqlite3_bind_blob(stmt, 1, id.data(), static_cast<int>(id.size()), SQLITE_TRANSIENT) !=
          SQLITE_OK ||
      sqlite3_bind_blob(stmt, 2, receipt.request_hash.data(),
                        static_cast<int>(receipt.request_hash.size()),
                        SQLITE_TRANSIENT) != SQLITE_OK ||
      sqlite3_bind_int64(stmt, 3, receipt.dbid) != SQLITE_OK ||
      sqlite3_bind_int64(stmt, 4, static_cast<int64_t>(receipt.checkout_version)) != SQLITE_OK ||
      sqlite3_step(stmt) != SQLITE_DONE)
    return SqliteError("receipt insert failed");
  return {};
}

void SqliteDatabase::FinishWrite(const std::string& scope, const DatabaseWriteOperation* operation,
                                 const std::optional<Sha256Digest>& hash, PutResult result,
                                 std::function<void(PutResult)> callback) {
  if (operation) {
    const DatabaseOperationReceipt receipt{
        *hash, result.dbid, result.checkout_owner ? result.checkout_owner->ownership_version : 0};
    auto saved = SaveOperationReceipt(operation->id, receipt);
    if (!saved) {
      RollbackWriteScope(scope);
      result.success = false;
      result.checkout_owner.reset();
      result.error = std::string(saved.Error().Message());
      FireOrDefer(std::move(callback), std::move(result));
      return;
    }
  }
  if (auto committed = CommitWriteScope(scope); !committed) {
    result.success = false;
    result.checkout_owner.reset();
    result.error = std::string(committed.Error().Message());
    result.error_kind = DatabaseErrorKind::kCommitUnknown;
  }
  FireOrDefer(std::move(callback), std::move(result));
}

void SqliteDatabase::CheckoutWithOperation(const DatabaseCheckoutOperation& operation,
                                           std::function<void(GetResult)> callback) {
  CheckoutOperationImpl(operation, true, std::move(callback));
}

void SqliteDatabase::ResolveCheckoutOperation(const DatabaseCheckoutOperation& operation,
                                              std::function<void(GetResult)> callback) {
  CheckoutOperationImpl(operation, false, std::move(callback));
}

void SqliteDatabase::CheckoutOperationImpl(const DatabaseCheckoutOperation& operation, bool acquire,
                                           std::function<void(GetResult)> callback) {
  GetResult result;
  if (!started_ || db_ == nullptr) {
    result.error = "sqlite backend not started";
    FireOrDefer(std::move(callback), std::move(result));
    return;
  }
  if (!operation.IsValid()) {
    result.error = "invalid_checkout_operation";
    result.error_kind = DatabaseErrorKind::kOperationConflict;
    FireOrDefer(std::move(callback), std::move(result));
    return;
  }
  auto hash = HashCheckoutOperation(operation);
  if (!hash) {
    result.error = hash.Error().Message();
    FireOrDefer(std::move(callback), std::move(result));
    return;
  }
  auto scope = BeginWriteScope();
  if (!scope) {
    result.error = scope.Error().Message();
    FireOrDefer(std::move(callback), std::move(result));
    return;
  }
  bool acquired = false;
  auto execute = [&]() -> GetResult {
    GetResult outcome;
    auto previous = FindOperationReceipt(operation.id);
    if (!previous) {
      outcome.error = previous.Error().Message();
      outcome.error_kind = DatabaseErrorKind::kCommitUnknown;
      return outcome;
    }
    if (*previous && (**previous).request_hash != *hash) {
      outcome.error = "operation_id_reused";
      outcome.error_kind = DatabaseErrorKind::kOperationConflict;
      return outcome;
    }
    if (!*previous && !acquire) {
      outcome.error = "checkout_operation_unconfirmed";
      outcome.error_kind = DatabaseErrorKind::kCommitUnknown;
      return outcome;
    }
    if (*previous) {
      if (auto valid = ValidateCheckoutReceipt(operation, **previous); !valid) {
        outcome.error = valid.Error().Message();
        outcome.error_kind = DatabaseErrorKind::kCommitUnknown;
        return outcome;
      }
    }
    if (!*previous) {
      if (auto active = CheckCheckoutProcess(operation.owner); !active) {
        outcome.error = active.Error().Message();
        outcome.error_kind = active.Error().Code() == ErrorCode::kPermissionDenied
                                 ? DatabaseErrorKind::kOwnershipConflict
                                 : DatabaseErrorKind::kNone;
        return outcome;
      }
    }
    auto row = *previous ? FetchByDbid((**previous).dbid, operation.type_id)
                         : (operation.dbid == kInvalidDBID
                                ? FetchByName(operation.type_id, operation.identifier)
                                : FetchByDbid(operation.dbid, operation.type_id));
    if (!row || !row->found) {
      outcome.error = row ? "checkout entity not found" : std::string(row.Error().Message());
      outcome.error_kind = row ? DatabaseErrorKind::kNotFound : DatabaseErrorKind::kNone;
      if (*previous && row) {
        outcome.data.dbid = (**previous).dbid;
        outcome.checkout_version = (**previous).checkout_version;
      }
      return outcome;
    }
    outcome.success = true;
    outcome.data = std::move(row->data);
    outcome.checked_out_by = row->checked_out_by;
    if (*previous) return ConfirmCheckoutReceipt(operation, **previous, std::move(outcome));
    if (outcome.checked_out_by) return outcome;
    auto version = AcquireCheckout(outcome.data.dbid, operation.type_id, operation.owner);
    if (!version) {
      outcome = GetResult{};
      outcome.error = version.Error().Message();
      return outcome;
    }
    auto saved = SaveOperationReceipt(operation.id, {*hash, outcome.data.dbid, *version});
    if (!saved) {
      outcome = GetResult{};
      outcome.error = saved.Error().Message();
      return outcome;
    }
    outcome.checkout_version = *version;
    acquired = true;
    return outcome;
  };
  result = execute();
  if (acquired) {
    if (auto committed = CommitWriteScope(*scope); !committed) {
      result = GetResult{};
      result.error = committed.Error().Message();
      result.error_kind = DatabaseErrorKind::kCommitUnknown;
    }
  } else {
    RollbackWriteScope(*scope);
  }
  FireOrDefer(std::move(callback), std::move(result));
}

}  // namespace atlas
