#include "db_sqlite/sqlite_database.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <limits>
#include <string_view>

#include "foundation/log.h"

namespace {

auto UnixTimeMs() -> int64_t {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

constexpr int kSqliteSchemaVersion = 1;

auto ConstraintErrorKind(sqlite3* db) -> atlas::DatabaseErrorKind {
  switch (sqlite3_extended_errcode(db)) {
    case SQLITE_CONSTRAINT_PRIMARYKEY:
    case SQLITE_CONSTRAINT_ROWID:
      return atlas::DatabaseErrorKind::kDuplicateDbid;
    case SQLITE_CONSTRAINT_UNIQUE:
      return atlas::DatabaseErrorKind::kDuplicateIdentifier;
    default:
      return atlas::DatabaseErrorKind::kNone;
  }
}

}  // namespace

namespace atlas {

SqliteDatabase::Statement::Statement(Statement&& other) noexcept : stmt_(other.stmt_) {
  other.stmt_ = nullptr;
}

auto SqliteDatabase::Statement::operator=(Statement&& other) noexcept -> Statement& {
  if (this == &other) {
    return *this;
  }
  Reset();
  stmt_ = other.stmt_;
  other.stmt_ = nullptr;
  return *this;
}

SqliteDatabase::Statement::~Statement() {
  Reset();
}

void SqliteDatabase::Statement::Reset() {
  if (stmt_ != nullptr) {
    sqlite3_finalize(stmt_);
  }
  stmt_ = nullptr;
}

SqliteDatabase::~SqliteDatabase() {
  if (started_) {
    Shutdown();
  }
}

auto SqliteDatabase::Startup(const DatabaseConfig& config, const EntityDefRegistry& entity_defs)
    -> Result<void> {
  if (started_ || db_ != nullptr)
    return Error{ErrorCode::kAlreadyExists, "SqliteDatabase is already started"};
  entity_defs_ = &entity_defs;
  db_path_ = config.sqlite_path;

  auto open_result = OpenDatabase(config);
  if (!open_result) {
    Shutdown();
    return open_result.Error();
  }

  auto schema_result = EnsureSchema();
  if (!schema_result) {
    Shutdown();
    return schema_result.Error();
  }

  started_ = true;
  ATLAS_LOG_INFO("SqliteDatabase: started at '{}'", db_path_.string());
  return {};
}

void SqliteDatabase::Shutdown() {
  started_ = false;
  EndBatch();
  while (!deferred_.empty()) ProcessResults();
  if (db_ != nullptr) {
    sqlite3_close(db_);
    db_ = nullptr;
  }
  database_lock_ = fs::ScopedFileLock{};
}

void SqliteDatabase::PutEntity(DatabaseID dbid, uint16_t type_id, WriteFlags flags,
                               std::span<const std::byte> blob, const std::string& identifier,
                               std::function<void(PutResult)> callback) {
  PutEntityImpl(nullptr, dbid, type_id, flags, blob, identifier, std::move(callback));
}

void SqliteDatabase::PutEntityWithOperation(const DatabaseWriteOperation& operation,
                                            DatabaseID dbid, uint16_t type_id, WriteFlags flags,
                                            std::span<const std::byte> blob,
                                            const std::string& identifier,
                                            std::function<void(PutResult)> callback) {
  const bool allocated_create = HasFlag(operation.requested_flags, WriteFlags::kCreateNew) &&
                                operation.requested_dbid == kInvalidDBID;
  if (!operation.IsValid() || (!allocated_create && dbid != operation.requested_dbid) ||
      (flags != operation.requested_flags &&
       !(allocated_create && flags == (operation.requested_flags | WriteFlags::kExplicitDbid))) ||
      (operation.expected_owner &&
       (HasFlag(flags, WriteFlags::kCreateNew) || dbid == kInvalidDBID))) {
    PutResult failed;
    failed.error = "invalid_or_unsupported_write_operation";
    failed.error_kind = DatabaseErrorKind::kOperationConflict;
    FireOrDefer(std::move(callback), std::move(failed));
    return;
  }
  if (operation.create_owner &&
      (!HasFlag(flags, WriteFlags::kCreateNew) || operation.expected_owner ||
       operation.create_owner->ownership_version != 0 ||
       operation.create_owner->base_addr.Port() == 0 || operation.entity_id == 0 ||
       operation.create_owner->entity_id != operation.entity_id ||
       HasFlag(flags, WriteFlags::kLogOff) || HasFlag(flags, WriteFlags::kDelete))) {
    PutResult failed;
    failed.error = "invalid_create_owner";
    failed.error_kind = DatabaseErrorKind::kOperationConflict;
    FireOrDefer(std::move(callback), std::move(failed));
    return;
  }
  PutEntityImpl(&operation, dbid, type_id, flags, blob, identifier, std::move(callback));
}

void SqliteDatabase::PutEntityImpl(const DatabaseWriteOperation* operation, DatabaseID dbid,
                                   uint16_t type_id, WriteFlags flags,
                                   std::span<const std::byte> blob, const std::string& identifier,
                                   std::function<void(PutResult)> callback) {
  PutResult result;

  if (!started_ || db_ == nullptr) {
    result.error = "sqlite backend not started";
    FireOrDefer(std::move(callback), std::move(result));
    return;
  }

  if (HasFlag(flags, WriteFlags::kDelete) && operation == nullptr) {
    DelEntity(dbid, type_id, [cb = std::move(callback)](DelResult del) mutable {
      PutResult put;
      put.success = del.success;
      put.error = std::move(del.error);
      cb(std::move(put));
    });
    return;
  }

  std::optional<Sha256Digest> hash;
  if (operation) {
    auto computed = HashWriteOperation(*operation, type_id, blob, identifier);
    if (!computed) {
      result.error = std::string(computed.Error().Message());
      FireOrDefer(std::move(callback), std::move(result));
      return;
    }
    hash = *computed;
  }
  auto scope_result = BeginWriteScope();
  if (!scope_result) {
    result.error = std::string(scope_result.Error().Message());
    FireOrDefer(std::move(callback), std::move(result));
    return;
  }
  auto scope = std::move(*scope_result);
  if (operation) {
    auto previous = FindOperationReceipt(operation->id);
    if (!previous || previous->has_value()) {
      if (!previous) {
        result.error = std::string(previous.Error().Message());
        result.error_kind = DatabaseErrorKind::kCommitUnknown;
      } else if ((**previous).request_hash != *hash) {
        result.error = "operation_id_reused";
        result.error_kind = DatabaseErrorKind::kOperationConflict;
      } else if ((operation->create_owner.has_value()) != ((**previous).checkout_version != 0)) {
        result.error = "invalid write receipt ownership";
        result.error_kind = DatabaseErrorKind::kCommitUnknown;
      } else {
        result = RestoreWriteReceipt(*operation, **previous);
      }
      RollbackWriteScope(scope);
      FireOrDefer(std::move(callback), std::move(result));
      return;
    }
  }

  if (operation && operation->create_owner) {
    if (auto active = CheckCheckoutProcess(*operation->create_owner); !active) {
      RollbackWriteScope(scope);
      result.error = active.Error().Message();
      result.error_kind = active.Error().Code() == ErrorCode::kPermissionDenied
                              ? DatabaseErrorKind::kOwnershipConflict
                              : DatabaseErrorKind::kNone;
      FireOrDefer(std::move(callback), std::move(result));
      return;
    }
  }
  if (HasFlag(flags, WriteFlags::kCreateNew) || dbid == kInvalidDBID) {
    const bool kUseExplicitDbid = HasFlag(flags, WriteFlags::kExplicitDbid) && dbid != kInvalidDBID;
    const auto kNowMs = UnixTimeMs();
    auto stmt_result =
        Prepare(kUseExplicitDbid
                    ? "INSERT INTO entities "
                      "(dbid, type_id, blob, identifier, password_hash, auto_load, checked_out, "
                      "checkout_ip, checkout_port, checkout_app_id, checkout_eid, "
                      "created_at_ms, updated_at_ms) "
                      "VALUES (?, ?, ?, ?, NULL, ?, 0, 0, 0, 0, 0, ?, ?)"
                    : "INSERT INTO entities "
                      "(type_id, blob, identifier, password_hash, auto_load, checked_out, "
                      "checkout_ip, checkout_port, checkout_app_id, checkout_eid, "
                      "created_at_ms, updated_at_ms) "
                      "VALUES (?, ?, ?, NULL, ?, 0, 0, 0, 0, 0, ?, ?)");
    if (!stmt_result) {
      result.error = std::string(stmt_result.Error().Message());
      RollbackWriteScope(scope);
      FireOrDefer(std::move(callback), std::move(result));
      return;
    }

    auto stmt = std::move(*stmt_result);
    int bind_index = 1;
    auto rc = SQLITE_OK;
    if (kUseExplicitDbid) {
      rc = sqlite3_bind_int64(stmt.Get(), bind_index++, dbid);
    }
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt.Get(), bind_index++, static_cast<int>(type_id));
    if (rc == SQLITE_OK) {
      auto bind_result = BindBlob(stmt, bind_index++, blob);
      rc = bind_result ? SQLITE_OK : 1;
      if (!bind_result) result.error = std::string(bind_result.Error().Message());
    }
    if (rc == SQLITE_OK) {
      auto bind_result = BindIdentifier(stmt, bind_index++, identifier);
      rc = bind_result ? SQLITE_OK : 1;
      if (!bind_result) result.error = std::string(bind_result.Error().Message());
    }
    if (rc == SQLITE_OK)
      rc = sqlite3_bind_int(stmt.Get(), bind_index++,
                            HasFlag(flags, WriteFlags::kAutoLoadOn) ? 1 : 0);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt.Get(), bind_index++, kNowMs);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt.Get(), bind_index++, kNowMs);

    if (rc != SQLITE_OK) {
      if (result.error.empty())
        result.error = std::string(SqliteError("SqliteDatabase: insert bind failed").Message());
      RollbackWriteScope(scope);
      FireOrDefer(std::move(callback), std::move(result));
      return;
    }

    rc = sqlite3_step(stmt.Get());
    if (rc != SQLITE_DONE) {
      result.error_kind = ConstraintErrorKind(db_);
      result.error = std::string(SqliteError("SqliteDatabase: insert failed", rc).Message());
      RollbackWriteScope(scope);
      FireOrDefer(std::move(callback), std::move(result));
      return;
    }

    result.dbid = kUseExplicitDbid ? dbid : static_cast<DatabaseID>(sqlite3_last_insert_rowid(db_));
    if (operation && operation->create_owner) {
      auto version = AcquireCheckout(result.dbid, type_id, *operation->create_owner);
      if (!version) {
        RollbackWriteScope(scope);
        result.error = std::string(version.Error().Message());
        FireOrDefer(std::move(callback), std::move(result));
        return;
      }
      result.checkout_owner = operation->create_owner;
      result.checkout_owner->ownership_version = *version;
    }
    result.success = true;
    FinishWrite(scope, operation, hash, std::move(result), std::move(callback));
    return;
  }


  auto row_result = FetchByDbid(dbid, type_id);
  if (!row_result) {
    RollbackWriteScope(scope);
    result.error = std::string(row_result.Error().Message());
    FireOrDefer(std::move(callback), std::move(result));
    return;
  }

  auto row = std::move(*row_result);
  if (!row.found) {
    RollbackWriteScope(scope);
    result.error_kind = DatabaseErrorKind::kNotFound;
    result.error = std::format("entity ({},{}) not found", type_id, dbid);
    FireOrDefer(std::move(callback), std::move(result));
    return;
  }

  if (operation &&
      (row.checked_out_by != operation->expected_owner ||
       (operation->expected_owner && operation->expected_owner->ownership_version == 0))) {
    RollbackWriteScope(scope);
    result.error = "checkout_owner_mismatch";
    result.error_kind = DatabaseErrorKind::kOwnershipConflict;
    FireOrDefer(std::move(callback), std::move(result));
    return;
  }
  if (HasFlag(flags, WriteFlags::kDelete)) {
    auto statement = Prepare("DELETE FROM entities WHERE dbid = ? AND type_id = ?");
    if (statement && sqlite3_bind_int64(statement->Get(), 1, dbid) == SQLITE_OK &&
        sqlite3_bind_int(statement->Get(), 2, type_id) == SQLITE_OK &&
        sqlite3_step(statement->Get()) == SQLITE_DONE) {
      result.success = true;
      result.dbid = dbid;
      FinishWrite(scope, operation, hash, std::move(result), std::move(callback));
      return;
    } else {
      result.error = std::string(SqliteError("conditional delete failed").Message());
      RollbackWriteScope(scope);
    }
    FireOrDefer(std::move(callback), std::move(result));
    return;
  }

  const std::string kFinalIdentifier = identifier.empty() ? row.data.identifier : identifier;
  const bool kFinalAutoLoad =
      HasFlag(flags, WriteFlags::kAutoLoadOn)
          ? true
          : (HasFlag(flags, WriteFlags::kAutoLoadOff) ? false : row.auto_load);
  const bool kFinalCheckedOut =
      HasFlag(flags, WriteFlags::kLogOff) ? false : row.checked_out_by.has_value();
  const CheckoutInfo kFinalOwner = row.checked_out_by.value_or(CheckoutInfo{});
  const auto kNowMs = UnixTimeMs();

  auto stmt_result = Prepare(
      "UPDATE entities SET blob = ?, identifier = ?, auto_load = ?, checked_out = ?, "
      "checkout_ip = ?, checkout_port = ?, checkout_app_id = ?, checkout_eid = ?, "
      "updated_at_ms = ? WHERE dbid = ? AND type_id = ?");
  if (!stmt_result) {
    RollbackWriteScope(scope);
    result.error = std::string(stmt_result.Error().Message());
    FireOrDefer(std::move(callback), std::move(result));
    return;
  }

  auto stmt = std::move(*stmt_result);
  auto rc = SQLITE_OK;
  auto bind_blob_result = BindBlob(stmt, 1, blob);
  rc = bind_blob_result ? SQLITE_OK : 1;
  if (!bind_blob_result) result.error = std::string(bind_blob_result.Error().Message());
  if (rc == SQLITE_OK) {
    auto bind_result = BindIdentifier(stmt, 2, kFinalIdentifier);
    rc = bind_result ? SQLITE_OK : 1;
    if (!bind_result) result.error = std::string(bind_result.Error().Message());
  }
  if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt.Get(), 3, kFinalAutoLoad ? 1 : 0);
  if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt.Get(), 4, kFinalCheckedOut ? 1 : 0);
  if (rc == SQLITE_OK)
    rc = sqlite3_bind_int64(stmt.Get(), 5, kFinalCheckedOut ? kFinalOwner.base_addr.Ip() : 0);
  if (rc == SQLITE_OK)
    rc = sqlite3_bind_int(stmt.Get(), 6,
                          kFinalCheckedOut ? static_cast<int>(kFinalOwner.base_addr.Port()) : 0);
  if (rc == SQLITE_OK)
    rc = sqlite3_bind_int(stmt.Get(), 7,
                          kFinalCheckedOut ? static_cast<int>(kFinalOwner.app_id) : 0);
  if (rc == SQLITE_OK)
    rc = sqlite3_bind_int(stmt.Get(), 8,
                          kFinalCheckedOut ? static_cast<int>(kFinalOwner.entity_id) : 0);
  if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt.Get(), 9, kNowMs);
  if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt.Get(), 10, dbid);
  if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt.Get(), 11, static_cast<int>(type_id));

  if (rc != SQLITE_OK) {
    RollbackWriteScope(scope);
    if (result.error.empty())
      result.error = std::string(SqliteError("SqliteDatabase: update bind failed").Message());
    FireOrDefer(std::move(callback), std::move(result));
    return;
  }

  rc = sqlite3_step(stmt.Get());
  if (rc != SQLITE_DONE) {
    RollbackWriteScope(scope);
    result.error = std::string(SqliteError("SqliteDatabase: update failed", rc).Message());
    FireOrDefer(std::move(callback), std::move(result));
    return;
  }

  result.success = true;
  result.dbid = dbid;
  FinishWrite(scope, operation, hash, std::move(result), std::move(callback));
}

void SqliteDatabase::PutEntityWithPassword(DatabaseID dbid, uint16_t type_id, WriteFlags flags,
                                           std::span<const std::byte> blob,
                                           const std::string& identifier,
                                           const std::string& password_hash,
                                           std::function<void(PutResult)> callback) {
  PutResult result;

  if (!started_ || db_ == nullptr) {
    result.error = "sqlite backend not started";
    FireOrDefer(std::move(callback), std::move(result));
    return;
  }

  if (!(HasFlag(flags, WriteFlags::kCreateNew) || dbid == kInvalidDBID)) {
    auto scope_result = BeginWriteScope();
    if (!scope_result) {
      result.error = std::string(scope_result.Error().Message());
      FireOrDefer(std::move(callback), std::move(result));
      return;
    }
    auto scope = std::move(*scope_result);

    auto row_result = FetchByDbid(dbid, type_id);
    if (!row_result) {
      RollbackWriteScope(scope);
      result.error = std::string(row_result.Error().Message());
      FireOrDefer(std::move(callback), std::move(result));
      return;
    }
    if (!row_result->found) {
      RollbackWriteScope(scope);
      result.error_kind = DatabaseErrorKind::kNotFound;
      result.error = std::format("entity ({},{}) not found", type_id, dbid);
      FireOrDefer(std::move(callback), std::move(result));
      return;
    }

    const std::string kFinalIdentifier =
        identifier.empty() ? row_result->data.identifier : identifier;
    const bool kFinalAutoLoad =
        HasFlag(flags, WriteFlags::kAutoLoadOn)
            ? true
            : (HasFlag(flags, WriteFlags::kAutoLoadOff) ? false : row_result->auto_load);
    const bool kFinalCheckedOut =
        HasFlag(flags, WriteFlags::kLogOff) ? false : row_result->checked_out_by.has_value();
    const CheckoutInfo kFinalOwner = row_result->checked_out_by.value_or(CheckoutInfo{});
    const auto kNowMs = UnixTimeMs();

    auto stmt_result = Prepare(
        "UPDATE entities SET blob = ?, identifier = ?, password_hash = ?, "
        "auto_load = ?, checked_out = ?, checkout_ip = ?, checkout_port = ?, "
        "checkout_app_id = ?, checkout_eid = ?, updated_at_ms = ? "
        "WHERE dbid = ? AND type_id = ?");
    if (!stmt_result) {
      RollbackWriteScope(scope);
      result.error = std::string(stmt_result.Error().Message());
      FireOrDefer(std::move(callback), std::move(result));
      return;
    }

    auto stmt = std::move(*stmt_result);
    auto rc = SQLITE_OK;
    auto bind_blob_result = BindBlob(stmt, 1, blob);
    rc = bind_blob_result ? SQLITE_OK : 1;
    if (!bind_blob_result) result.error = std::string(bind_blob_result.Error().Message());
    if (rc == SQLITE_OK) {
      auto bind_identifier_result = BindIdentifier(stmt, 2, kFinalIdentifier);
      rc = bind_identifier_result ? SQLITE_OK : 1;
      if (!bind_identifier_result)
        result.error = std::string(bind_identifier_result.Error().Message());
    }
    if (rc == SQLITE_OK) {
      if (password_hash.empty())
        rc = sqlite3_bind_null(stmt.Get(), 3);
      else
        rc = sqlite3_bind_text(stmt.Get(), 3, password_hash.c_str(), -1, SQLITE_TRANSIENT);
    }
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt.Get(), 4, kFinalAutoLoad ? 1 : 0);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt.Get(), 5, kFinalCheckedOut ? 1 : 0);
    if (rc == SQLITE_OK)
      rc = sqlite3_bind_int64(stmt.Get(), 6, kFinalCheckedOut ? kFinalOwner.base_addr.Ip() : 0);
    if (rc == SQLITE_OK)
      rc = sqlite3_bind_int(stmt.Get(), 7,
                            kFinalCheckedOut ? static_cast<int>(kFinalOwner.base_addr.Port()) : 0);
    if (rc == SQLITE_OK)
      rc = sqlite3_bind_int(stmt.Get(), 8,
                            kFinalCheckedOut ? static_cast<int>(kFinalOwner.app_id) : 0);
    if (rc == SQLITE_OK)
      rc = sqlite3_bind_int(stmt.Get(), 9,
                            kFinalCheckedOut ? static_cast<int>(kFinalOwner.entity_id) : 0);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt.Get(), 10, kNowMs);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt.Get(), 11, dbid);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt.Get(), 12, static_cast<int>(type_id));

    if (rc != SQLITE_OK || sqlite3_step(stmt.Get()) != SQLITE_DONE) {
      RollbackWriteScope(scope);
      if (result.error.empty())
        result.error = std::string(SqliteError("SqliteDatabase: password update failed").Message());
      FireOrDefer(std::move(callback), std::move(result));
      return;
    }

    if (auto committed = CommitWriteScope(scope); !committed) {
      result.error = std::string(committed.Error().Message());
      result.error_kind = DatabaseErrorKind::kCommitUnknown;
      FireOrDefer(std::move(callback), std::move(result));
      return;
    }

    result.success = true;
    result.dbid = dbid;
    FireOrDefer(std::move(callback), std::move(result));
    return;
  }

  const bool kUseExplicitDbid = HasFlag(flags, WriteFlags::kExplicitDbid) && dbid != kInvalidDBID;
  const auto kNowMs = UnixTimeMs();
  auto stmt_result = Prepare(
      kUseExplicitDbid ? "INSERT INTO entities "
                         "(dbid, type_id, blob, identifier, password_hash, auto_load, checked_out, "
                         "checkout_ip, checkout_port, checkout_app_id, checkout_eid, "
                         "created_at_ms, updated_at_ms) "
                         "VALUES (?, ?, ?, ?, ?, ?, 0, 0, 0, 0, 0, ?, ?)"
                       : "INSERT INTO entities "
                         "(type_id, blob, identifier, password_hash, auto_load, checked_out, "
                         "checkout_ip, checkout_port, checkout_app_id, checkout_eid, "
                         "created_at_ms, updated_at_ms) "
                         "VALUES (?, ?, ?, ?, ?, 0, 0, 0, 0, 0, ?, ?)");
  if (!stmt_result) {
    result.error = std::string(stmt_result.Error().Message());
    FireOrDefer(std::move(callback), std::move(result));
    return;
  }

  auto stmt = std::move(*stmt_result);
  int bind_index = 1;
  auto rc = SQLITE_OK;
  if (kUseExplicitDbid) {
    rc = sqlite3_bind_int64(stmt.Get(), bind_index++, dbid);
  }
  if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt.Get(), bind_index++, static_cast<int>(type_id));
  if (rc == SQLITE_OK) {
    auto bind_result = BindBlob(stmt, bind_index++, blob);
    rc = bind_result ? SQLITE_OK : 1;
    if (!bind_result) result.error = std::string(bind_result.Error().Message());
  }
  if (rc == SQLITE_OK) {
    auto bind_result = BindIdentifier(stmt, bind_index++, identifier);
    rc = bind_result ? SQLITE_OK : 1;
    if (!bind_result) result.error = std::string(bind_result.Error().Message());
  }
  if (rc == SQLITE_OK) {
    if (password_hash.empty())
      rc = sqlite3_bind_null(stmt.Get(), bind_index++);
    else
      rc = sqlite3_bind_text(stmt.Get(), bind_index++, password_hash.c_str(), -1, SQLITE_TRANSIENT);
  }
  if (rc == SQLITE_OK)
    rc =
        sqlite3_bind_int(stmt.Get(), bind_index++, HasFlag(flags, WriteFlags::kAutoLoadOn) ? 1 : 0);
  if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt.Get(), bind_index++, kNowMs);
  if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt.Get(), bind_index++, kNowMs);

  if (rc != SQLITE_OK) {
    if (result.error.empty())
      result.error =
          std::string(SqliteError("SqliteDatabase: password insert bind failed").Message());
    FireOrDefer(std::move(callback), std::move(result));
    return;
  }

  rc = sqlite3_step(stmt.Get());
  if (rc != SQLITE_DONE) {
    result.error_kind = ConstraintErrorKind(db_);
    result.error = std::string(SqliteError("SqliteDatabase: password insert failed", rc).Message());
    FireOrDefer(std::move(callback), std::move(result));
    return;
  }

  result.success = true;
  result.dbid = kUseExplicitDbid ? dbid : static_cast<DatabaseID>(sqlite3_last_insert_rowid(db_));
  FireOrDefer(std::move(callback), std::move(result));
}

void SqliteDatabase::GetEntity(DatabaseID dbid, uint16_t type_id,
                               std::function<void(GetResult)> callback) {
  GetResult result;
  auto row_result = FetchByDbid(dbid, type_id);
  if (!row_result) {
    result.error = std::string(row_result.Error().Message());
    FireOrDefer(std::move(callback), std::move(result));
    return;
  }

  auto row = std::move(*row_result);
  if (!row.found) {
    result.error_kind = DatabaseErrorKind::kNotFound;
    result.error = std::format("entity ({},{}) not found", type_id, dbid);
    FireOrDefer(std::move(callback), std::move(result));
    return;
  }

  result.success = true;
  result.data = std::move(row.data);
  result.checked_out_by = row.checked_out_by;
  FireOrDefer(std::move(callback), std::move(result));
}

void SqliteDatabase::DelEntity(DatabaseID dbid, uint16_t type_id,
                               std::function<void(DelResult)> callback) {
  DelResult result;
  auto stmt_result = Prepare("DELETE FROM entities WHERE dbid = ? AND type_id = ?");
  if (!stmt_result) {
    result.error = std::string(stmt_result.Error().Message());
    FireOrDefer(std::move(callback), std::move(result));
    return;
  }

  auto stmt = std::move(*stmt_result);
  auto rc = sqlite3_bind_int64(stmt.Get(), 1, dbid);
  if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt.Get(), 2, static_cast<int>(type_id));
  if (rc != SQLITE_OK) {
    result.error = std::string(SqliteError("SqliteDatabase: delete bind failed").Message());
    FireOrDefer(std::move(callback), std::move(result));
    return;
  }

  rc = sqlite3_step(stmt.Get());
  if (rc != SQLITE_DONE) {
    result.error = std::string(SqliteError("SqliteDatabase: delete failed", rc).Message());
    FireOrDefer(std::move(callback), std::move(result));
    return;
  }

  if (sqlite3_changes(db_) <= 0) {
    result.error = std::format("entity ({},{}) not found", type_id, dbid);
    FireOrDefer(std::move(callback), std::move(result));
    return;
  }

  result.success = true;
  FireOrDefer(std::move(callback), std::move(result));
}

void SqliteDatabase::LookupByName(uint16_t type_id, const std::string& identifier,
                                  std::function<void(LookupResult)> callback) {
  LookupResult result;
  auto row_result = FetchByName(type_id, identifier);
  if (!row_result) {
    result.error = std::string(row_result.Error().Message());
    FireOrDefer(std::move(callback), std::move(result));
    return;
  }

  auto row = std::move(*row_result);
  if (row.found) {
    result.found = true;
    result.dbid = row.data.dbid;
    result.password_hash = std::move(row.password_hash);
  }
  FireOrDefer(std::move(callback), std::move(result));
}

void SqliteDatabase::CheckoutEntity(DatabaseID dbid, uint16_t type_id,
                                    const CheckoutInfo& new_owner,
                                    std::function<void(GetResult)> callback) {
  GetResult result;
  auto scope = BeginWriteScope();
  if (!scope) {
    result.error = std::string(scope.Error().Message());
    FireOrDefer(std::move(callback), std::move(result));
    return;
  }
  if (auto active = CheckCheckoutProcess(new_owner); !active) {
    RollbackWriteScope(*scope);
    result.error = active.Error().Message();
    result.error_kind = active.Error().Code() == ErrorCode::kPermissionDenied
                            ? DatabaseErrorKind::kOwnershipConflict
                            : DatabaseErrorKind::kNone;
    FireOrDefer(std::move(callback), std::move(result));
    return;
  }
  auto row = FetchByDbid(dbid, type_id);
  if (!row || !row->found) {
    RollbackWriteScope(*scope);
    result.error_kind = row ? DatabaseErrorKind::kNotFound : DatabaseErrorKind::kNone;
    result.error = row ? std::format("checkout: entity ({},{}) not found", type_id, dbid)
                       : std::string(row.Error().Message());
    FireOrDefer(std::move(callback), std::move(result));
    return;
  }
  CheckoutTail(std::move(*row), new_owner, *scope, std::move(callback));
}

void SqliteDatabase::CheckoutEntityByName(uint16_t type_id, const std::string& identifier,
                                          const CheckoutInfo& new_owner,
                                          std::function<void(GetResult)> callback) {
  GetResult result;
  auto scope = BeginWriteScope();
  if (!scope) {
    result.error = std::string(scope.Error().Message());
    FireOrDefer(std::move(callback), std::move(result));
    return;
  }
  if (auto active = CheckCheckoutProcess(new_owner); !active) {
    RollbackWriteScope(*scope);
    result.error = active.Error().Message();
    result.error_kind = active.Error().Code() == ErrorCode::kPermissionDenied
                            ? DatabaseErrorKind::kOwnershipConflict
                            : DatabaseErrorKind::kNone;
    FireOrDefer(std::move(callback), std::move(result));
    return;
  }
  auto row = FetchByName(type_id, identifier);
  if (!row || !row->found) {
    RollbackWriteScope(*scope);
    result.error_kind = row ? DatabaseErrorKind::kNotFound : DatabaseErrorKind::kNone;
    result.error = row ? std::format("checkout_by_name: '{}' not found", identifier)
                       : std::string(row.Error().Message());
    FireOrDefer(std::move(callback), std::move(result));
    return;
  }
  CheckoutTail(std::move(*row), new_owner, *scope, std::move(callback));
}

auto SqliteDatabase::AcquireCheckoutVersion(DatabaseID dbid, uint16_t type_id) -> Result<uint64_t> {
  auto prepared = Prepare(
      "INSERT INTO atlas_checkout_versions(dbid, type_id, version) VALUES(?, ?, 1) "
      "ON CONFLICT(dbid, type_id) DO UPDATE SET version = version + 1 "
      "WHERE version > 0 AND version < 9223372036854775807 RETURNING version");
  if (!prepared) return prepared.Error();
  auto stmt = std::move(*prepared);
  auto rc = sqlite3_bind_int64(stmt.Get(), 1, dbid);
  if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt.Get(), 2, type_id);
  if (rc != SQLITE_OK) return SqliteError("checkout version bind failed", rc);
  rc = sqlite3_step(stmt.Get());
  if (rc == SQLITE_DONE) return Error{ErrorCode::kOutOfRange, "checkout version exhausted"};
  if (rc != SQLITE_ROW) return SqliteError("checkout version reservation failed", rc);
  const auto version = sqlite3_column_int64(stmt.Get(), 0);
  if (sqlite3_column_type(stmt.Get(), 0) != SQLITE_INTEGER || version <= 0) {
    return Error{ErrorCode::kIoError, "invalid checkout version"};
  }
  return static_cast<uint64_t>(version);
}

auto SqliteDatabase::AcquireCheckout(DatabaseID dbid, uint16_t type_id, const CheckoutInfo& owner)
    -> Result<uint64_t> {
  auto version = AcquireCheckoutVersion(dbid, type_id);
  if (!version) return version.Error();
  auto prepared = Prepare(
      "UPDATE entities SET checked_out = 1, checkout_ip = ?, checkout_port = ?, "
      "checkout_app_id = ?, checkout_eid = ?, updated_at_ms = ?, checkout_version = ?, "
      "checkout_instance = ? "
      "WHERE dbid = ? AND type_id = ? AND checked_out = 0");
  if (!prepared) return prepared.Error();
  auto stmt = std::move(*prepared);
  auto rc = sqlite3_bind_int64(stmt.Get(), 1, owner.base_addr.Ip());
  if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt.Get(), 2, owner.base_addr.Port());
  if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt.Get(), 3, owner.app_id);
  if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt.Get(), 4, owner.entity_id);
  if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt.Get(), 5, UnixTimeMs());
  if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt.Get(), 6, static_cast<int64_t>(*version));
  if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt.Get(), 7, owner.process_instance);
  if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt.Get(), 8, dbid);
  if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt.Get(), 9, type_id);
  if (rc != SQLITE_OK || sqlite3_step(stmt.Get()) != SQLITE_DONE) {
    return SqliteError("checkout update failed");
  }
  if (sqlite3_changes(db_) != 1) {
    return Error{ErrorCode::kPermissionDenied, "checkout owner changed"};
  }
  return *version;
}

void SqliteDatabase::CheckoutTail(EntityRow row, const CheckoutInfo& owner,
                                  const std::string& scope,
                                  std::function<void(GetResult)> callback) {
  GetResult result;
  if (row.checked_out_by) {
    RollbackWriteScope(scope);
    result.success = true;
    result.data = std::move(row.data);
    result.checked_out_by = row.checked_out_by;
    FireOrDefer(std::move(callback), std::move(result));
    return;
  }
  auto version = AcquireCheckout(row.data.dbid, row.data.type_id, owner);
  if (!version) {
    RollbackWriteScope(scope);
    result.error = std::string(version.Error().Message());
  } else if (auto committed = CommitWriteScope(scope); !committed) {
    result.error = std::string(committed.Error().Message());
    result.error_kind = DatabaseErrorKind::kCommitUnknown;
  } else {
    result.success = true;
    result.data = std::move(row.data);
    result.checkout_version = *version;
  }
  FireOrDefer(std::move(callback), std::move(result));
}

void SqliteDatabase::ReleaseCheckout(DatabaseID dbid, uint16_t type_id, const CheckoutInfo& owner,
                                     std::function<void(Result<void>)> callback) {
  if (owner.ownership_version == 0 ||
      owner.ownership_version > static_cast<uint64_t>((std::numeric_limits<int64_t>::max)())) {
    FireOrDefer(std::move(callback),
                Result<void>{Error{ErrorCode::kInvalidArgument, "invalid checkout version"}});
    return;
  }
  auto scope = BeginWriteScope();
  if (!scope) {
    FireOrDefer(std::move(callback), Result<void>{scope.Error()});
    return;
  }
  auto release = [&]() -> Result<void> {
    auto prepared = Prepare(
        "UPDATE entities SET checked_out = 0, checkout_ip = 0, checkout_port = 0, "
        "checkout_app_id = 0, checkout_eid = 0, updated_at_ms = ? "
        "WHERE dbid = ? AND type_id = ? AND checked_out = 1 AND checkout_ip = ? "
        "AND checkout_port = ? AND checkout_app_id = ? AND checkout_eid = ? "
        "AND checkout_version = ? AND checkout_instance = ?");
    if (!prepared) return prepared.Error();
    auto stmt = std::move(*prepared);
    auto rc = sqlite3_bind_int64(stmt.Get(), 1, UnixTimeMs());
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt.Get(), 2, dbid);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt.Get(), 3, type_id);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt.Get(), 4, owner.base_addr.Ip());
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt.Get(), 5, owner.base_addr.Port());
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt.Get(), 6, owner.app_id);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt.Get(), 7, owner.entity_id);
    if (rc == SQLITE_OK)
      rc = sqlite3_bind_int64(stmt.Get(), 8, static_cast<int64_t>(owner.ownership_version));
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt.Get(), 9, owner.process_instance);
    if (rc != SQLITE_OK || sqlite3_step(stmt.Get()) != SQLITE_DONE) {
      return SqliteError("conditional checkout release failed");
    }
    if (sqlite3_changes(db_) != 1) {
      return Error{ErrorCode::kPermissionDenied, "checkout owner mismatch"};
    }
    return {};
  };
  auto result = release();
  if (result)
    result = CommitWriteScope(*scope);
  else
    RollbackWriteScope(*scope);
  FireOrDefer(std::move(callback), std::move(result));
}

auto SqliteDatabase::CheckCheckoutProcess(const CheckoutInfo& owner) -> Result<void> {
  if (owner.process_instance == 0) return {};
  if (owner.process_instance > static_cast<uint64_t>((std::numeric_limits<int64_t>::max)()))
    return Error{ErrorCode::kInvalidArgument, "invalid checkout process instance"};
  auto inserted = ExecSql(std::format(
      "INSERT INTO atlas_checkout_processes(base_ip, base_port, instance) VALUES({}, {}, {}) "
      "ON CONFLICT(base_ip, base_port, instance) DO NOTHING",
      owner.base_addr.Ip(), owner.base_addr.Port(), owner.process_instance));
  if (!inserted) return inserted.Error();
  auto prepared =
      Prepare(std::format("SELECT retired FROM atlas_checkout_processes "
                          "WHERE base_ip = {} AND base_port = {} AND instance = {}",
                          owner.base_addr.Ip(), owner.base_addr.Port(), owner.process_instance));
  if (!prepared) return prepared.Error();
  if (sqlite3_step(prepared->Get()) != SQLITE_ROW) return SqliteError("checkout process missing");
  if (sqlite3_column_int(prepared->Get(), 0) != 0)
    return Error{ErrorCode::kPermissionDenied, "checkout process retired"};
  return {};
}

void SqliteDatabase::RetireCheckoutProcess(const Address& base_addr, uint64_t instance,
                                           std::function<void(Result<int>)> callback) {
  if (instance == 0 || instance > static_cast<uint64_t>((std::numeric_limits<int64_t>::max)())) {
    FireOrDefer(std::move(callback), Result<int>{Error{ErrorCode::kInvalidArgument,
                                                       "invalid checkout process instance"}});
    return;
  }
  auto scope = BeginWriteScope();
  if (!scope) {
    FireOrDefer(std::move(callback), Result<int>{scope.Error()});
    return;
  }
  auto retire = [&]() -> Result<int> {
    auto marked = ExecSql(std::format(
        "INSERT INTO atlas_checkout_processes(base_ip, base_port, instance, retired) "
        "VALUES({}, {}, {}, 1) ON CONFLICT(base_ip, base_port, instance) DO UPDATE SET retired = 1",
        base_addr.Ip(), base_addr.Port(), instance));
    if (!marked) return marked.Error();
    auto cleared = ExecSql(
        std::format("UPDATE entities SET checked_out = 0, checkout_ip = 0, checkout_port = 0, "
                    "checkout_app_id = 0, checkout_eid = 0, checkout_instance = 0 "
                    "WHERE checked_out = 1 AND checkout_ip = {} AND checkout_port = {} "
                    "AND checkout_instance = {}",
                    base_addr.Ip(), base_addr.Port(), instance));
    if (!cleared) return cleared.Error();
    return sqlite3_changes(db_);
  };
  auto result = retire();
  if (result) {
    if (auto committed = CommitWriteScope(*scope); !committed) result = committed.Error();
  } else {
    RollbackWriteScope(*scope);
  }
  FireOrDefer(std::move(callback), std::move(result));
}

void SqliteDatabase::ClearCheckout(DatabaseID dbid, uint16_t type_id,
                                   std::function<void(bool)> callback) {
  bool cleared = false;
  const auto kNowMs = UnixTimeMs();
  auto stmt_result = Prepare(
      "UPDATE entities SET checked_out = 0, checkout_ip = 0, checkout_port = 0, "
      "checkout_app_id = 0, checkout_eid = 0, updated_at_ms = ? "
      "WHERE dbid = ? AND type_id = ? AND checked_out = 1");
  if (stmt_result) {
    auto stmt = std::move(*stmt_result);
    auto rc = sqlite3_bind_int64(stmt.Get(), 1, kNowMs);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt.Get(), 2, dbid);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt.Get(), 3, static_cast<int>(type_id));
    if (rc == SQLITE_OK && sqlite3_step(stmt.Get()) == SQLITE_DONE) {
      cleared = sqlite3_changes(db_) > 0;
    }
  }

  FireOrDefer(std::move(callback), std::move(cleared));
}

void SqliteDatabase::ClearCheckoutsForAddress(const Address& base_addr,
                                              std::function<void(int cleared_count)> callback) {
  int cleared = 0;
  const auto kNowMs = UnixTimeMs();
  auto stmt_result = Prepare(
      "UPDATE entities SET checked_out = 0, checkout_ip = 0, checkout_port = 0, "
      "checkout_app_id = 0, checkout_eid = 0, updated_at_ms = ? "
      "WHERE checked_out = 1 AND checkout_ip = ? AND checkout_port = ?");
  if (stmt_result) {
    auto stmt = std::move(*stmt_result);
    auto rc = sqlite3_bind_int64(stmt.Get(), 1, kNowMs);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt.Get(), 2, base_addr.Ip());
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt.Get(), 3, base_addr.Port());
    if (rc == SQLITE_OK && sqlite3_step(stmt.Get()) == SQLITE_DONE) {
      cleared = sqlite3_changes(db_);
    }
  }

  FireOrDefer(std::move(callback), std::move(cleared));
}

void SqliteDatabase::MarkCheckoutCleared(DatabaseID dbid, uint16_t type_id) {
  const auto kNowMs = UnixTimeMs();
  auto stmt_result = Prepare(
      "UPDATE entities SET checked_out = 0, checkout_ip = 0, checkout_port = 0, "
      "checkout_app_id = 0, checkout_eid = 0, updated_at_ms = ? "
      "WHERE dbid = ? AND type_id = ? AND checked_out = 1");
  if (!stmt_result) {
    return;
  }

  auto stmt = std::move(*stmt_result);
  auto rc = sqlite3_bind_int64(stmt.Get(), 1, kNowMs);
  if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt.Get(), 2, dbid);
  if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt.Get(), 3, static_cast<int>(type_id));
  if (rc == SQLITE_OK) {
    (void)sqlite3_step(stmt.Get());
  }
}

void SqliteDatabase::GetAutoLoadEntities(std::function<void(std::vector<EntityData>)> callback) {
  std::vector<EntityData> result;
  auto stmt_result = Prepare(
      "SELECT dbid, type_id, blob, identifier FROM entities WHERE auto_load = 1 "
      "ORDER BY type_id, dbid");
  if (!stmt_result) {
    FireOrDefer(std::move(callback), std::move(result));
    return;
  }

  auto stmt = std::move(*stmt_result);
  for (;;) {
    auto rc = sqlite3_step(stmt.Get());
    if (rc == SQLITE_DONE) {
      break;
    }
    if (rc != SQLITE_ROW) {
      break;
    }

    EntityData data;
    data.dbid = static_cast<DatabaseID>(sqlite3_column_int64(stmt.Get(), 0));
    data.type_id = static_cast<uint16_t>(sqlite3_column_int(stmt.Get(), 1));
    auto blob_ptr = sqlite3_column_blob(stmt.Get(), 2);
    auto blob_size = sqlite3_column_bytes(stmt.Get(), 2);
    if (blob_ptr != nullptr && blob_size > 0) {
      auto* bytes = static_cast<const std::byte*>(blob_ptr);
      data.blob.assign(bytes, bytes + blob_size);
    }
    if (const auto* text = sqlite3_column_text(stmt.Get(), 3); text != nullptr) {
      data.identifier = reinterpret_cast<const char*>(text);
    }
    result.push_back(std::move(data));
  }

  FireOrDefer(std::move(callback), std::move(result));
}

void SqliteDatabase::SetAutoLoad(DatabaseID dbid, uint16_t type_id, bool auto_load) {
  const auto kNowMs = UnixTimeMs();
  auto stmt_result = Prepare(
      "UPDATE entities SET auto_load = ?, updated_at_ms = ? WHERE dbid = ? AND type_id = ?");
  if (!stmt_result) {
    return;
  }

  auto stmt = std::move(*stmt_result);
  auto rc = sqlite3_bind_int(stmt.Get(), 1, auto_load ? 1 : 0);
  if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt.Get(), 2, kNowMs);
  if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt.Get(), 3, dbid);
  if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt.Get(), 4, static_cast<int>(type_id));
  if (rc == SQLITE_OK) {
    (void)sqlite3_step(stmt.Get());
  }
}

void SqliteDatabase::GetMaxDbidInRange(DatabaseID low, DatabaseID high,
                                       std::function<void(DbidRangeResult)> callback) {
  DbidRangeResult result;

  if (!started_ || db_ == nullptr) {
    result.error = "sqlite backend not started";
    FireOrDefer(std::move(callback), std::move(result));
    return;
  }

  if (high <= low) {
    result.success = true;
    FireOrDefer(std::move(callback), std::move(result));
    return;
  }

  auto stmt_result = Prepare(
      "SELECT MAX(dbid) FROM ("
      "SELECT MAX(dbid) AS dbid FROM entities WHERE dbid >= ?1 AND dbid < ?2 UNION ALL "
      "SELECT MAX(dbid) FROM atlas_checkout_versions WHERE dbid >= ?1 AND dbid < ?2 UNION ALL "
      "SELECT MAX(result_dbid) FROM atlas_write_receipts "
      "WHERE result_dbid >= ?1 AND result_dbid < ?2)");
  if (!stmt_result) {
    result.error = std::string(stmt_result.Error().Message());
    FireOrDefer(std::move(callback), std::move(result));
    return;
  }

  auto stmt = std::move(*stmt_result);
  auto rc = sqlite3_bind_int64(stmt.Get(), 1, low);
  if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt.Get(), 2, high);
  if (rc != SQLITE_OK) {
    result.error = std::string(SqliteError("SqliteDatabase: max dbid bind failed", rc).Message());
    FireOrDefer(std::move(callback), std::move(result));
    return;
  }

  rc = sqlite3_step(stmt.Get());
  if (rc == SQLITE_ROW) {
    result.success = true;
    if (sqlite3_column_type(stmt.Get(), 0) != SQLITE_NULL) {
      result.max_dbid = static_cast<DatabaseID>(sqlite3_column_int64(stmt.Get(), 0));
    }
  } else {
    result.error = std::string(SqliteError("SqliteDatabase: max dbid query failed", rc).Message());
  }

  FireOrDefer(std::move(callback), std::move(result));
}

void SqliteDatabase::ReserveEntityIds(uint32_t count,
                                      std::function<void(Result<EntityIdRange>)> callback) {
  if (batch_txn_open_) {
    deferred_.push_back([this, count, cb = std::move(callback)]() mutable {
      ReserveEntityIds(count, std::move(cb));
    });
    return;
  }
  auto reserve = [&]() -> Result<EntityIdRange> {
    if (!started_ || db_ == nullptr) {
      return Error{ErrorCode::kIoError, "SqliteDatabase is not running"};
    }
    if (count == 0 || count >= kFirstLocalEntityID) {
      return Error{ErrorCode::kOutOfRange, "invalid EntityID reservation count"};
    }
    auto begin = ExecSql("BEGIN IMMEDIATE");
    if (!begin) return begin.Error();
    auto update = [&]() -> Result<EntityIdRange> {
      auto prepared =
          Prepare(std::format("UPDATE atlas_entity_id_counter SET next_id = next_id + {} "
                              "WHERE id = 1 AND next_id >= 1 AND next_id <= {} "
                              "RETURNING next_id - {}, next_id - 1",
                              count, kFirstLocalEntityID - count, count));
      if (!prepared) return prepared.Error();
      auto stmt = std::move(*prepared);
      const auto status = sqlite3_step(stmt.Get());
      if (status == SQLITE_DONE) {
        return Error{ErrorCode::kOutOfRange, "EntityID counter is invalid or exhausted"};
      }
      if (status != SQLITE_ROW) return SqliteError("EntityID reservation failed", status);
      return EntityIdRange{static_cast<EntityID>(sqlite3_column_int64(stmt.Get(), 0)),
                           static_cast<EntityID>(sqlite3_column_int64(stmt.Get(), 1))};
    };
    auto range = update();
    if (!range) {
      (void)ExecSql("ROLLBACK");
      return range;
    }
    auto commit = ExecSql("COMMIT");
    if (!commit) {
      (void)ExecSql("ROLLBACK");
      return Error{ErrorCode::kCommitUnknown, std::string(commit.Error().Message())};
    }
    return range;
  };
  auto result = reserve();
  FireOrDefer(std::move(callback), std::move(result));
}

void SqliteDatabase::ProcessResults() {
  if (batch_txn_open_) return;
  int budget = kMaxCallbacksPerTick;
  while (!deferred_.empty() && budget-- > 0) {
    auto cb = std::move(deferred_.front());
    deferred_.pop_front();
    cb();
  }
}

void SqliteDatabase::BeginBatch() {
  batch_active_ = true;
  batch_savepoint_seq_ = 0;
}

void SqliteDatabase::EndBatch() {
  if (batch_txn_open_) {
    auto committed = batch_status_->error ? Result<void>{*batch_status_->error} : ExecSql("COMMIT");
    if (!committed) {
      batch_status_->error =
          Error{ErrorCode::kCommitUnknown, std::string(committed.Error().Message())};
      (void)ExecSql("ROLLBACK");
      ATLAS_LOG_ERROR("SqliteDatabase: batch commit failed: {}", committed.Error().Message());
    }
    batch_txn_open_ = false;
  }
  batch_active_ = false;
  batch_savepoint_seq_ = 0;
  batch_status_.reset();
  if (!deferred_mode_) ProcessResults();
}

auto SqliteDatabase::BeginWriteScope() -> Result<std::string> {
  if (!batch_active_) {
    auto r = ExecSql("BEGIN IMMEDIATE");
    if (!r) return r.Error();
    return std::string{};
  }

  if (!batch_txn_open_) {
    auto r = ExecSql("BEGIN IMMEDIATE");
    if (!r) return r.Error();
    batch_status_ = std::make_shared<BatchStatus>();
    batch_txn_open_ = true;
  }

  auto name = std::format("sp_{}", ++batch_savepoint_seq_);
  auto r = ExecSql(std::format("SAVEPOINT {}", name));
  if (!r) return r.Error();
  return name;
}

auto SqliteDatabase::CommitWriteScope(const std::string& scope) -> Result<void> {
  auto committed = ExecSql(scope.empty() ? "COMMIT" : std::format("RELEASE {}", scope));
  if (!committed) {
    if (batch_status_) batch_status_->error = committed.Error();
    if (scope.empty()) (void)ExecSql("ROLLBACK");
    return Error{ErrorCode::kCommitUnknown, std::string(committed.Error().Message())};
  }
  return {};
}

void SqliteDatabase::RollbackWriteScope(const std::string& scope) {
  if (scope.empty()) {
    (void)ExecSql("ROLLBACK");
  } else {
    (void)ExecSql(std::format("ROLLBACK TO {}", scope));
    (void)ExecSql(std::format("RELEASE {}", scope));
  }
}

auto SqliteDatabase::OpenDatabase(const DatabaseConfig& config) -> Result<void> {
  if (db_path_.empty())
    return Error{ErrorCode::kInvalidArgument, "SQLite requires an explicit database path"};
  auto parent = db_path_.parent_path();
  if (!parent.empty()) {
    std::error_code ec;
    std::filesystem::create_directories(parent, ec);
    if (ec) {
      return Error{ErrorCode::kIoError, std::format("SqliteDatabase: cannot create dir '{}': {}",
                                                    parent.string(), ec.message())};
    }
  }

  if (db_path_ != ":memory:") {
    std::error_code ec;
    auto canonical = std::filesystem::weakly_canonical(db_path_, ec);
    if (ec) return Error{ErrorCode::kIoError, "cannot resolve SQLite database path"};
    auto lock_path = canonical;
    lock_path += ".atlas-lock";
    auto locked = fs::ScopedFileLock::TryAcquire(lock_path);
    if (!locked)
      return Error{
          locked.Error().Code(),
          std::format("SQLite supports a single DBApp per store: {}", locked.Error().Message())};
    database_lock_ = std::move(*locked);
  }

  auto flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
  auto rc = sqlite3_open_v2(db_path_.string().c_str(), &db_, flags, nullptr);
  if (rc != SQLITE_OK || db_ == nullptr) {
    return SqliteError("SqliteDatabase: sqlite3_open_v2 failed", rc);
  }

  rc = sqlite3_busy_timeout(db_, config.sqlite_busy_timeout_ms);
  if (rc != SQLITE_OK) {
    auto err = SqliteError("SqliteDatabase: sqlite3_busy_timeout failed", rc);
    sqlite3_close(db_);
    db_ = nullptr;
    return err;
  }

  auto journal_sql =
      std::string(config.sqlite_wal ? "PRAGMA journal_mode = WAL" : "PRAGMA journal_mode = DELETE");
  auto pragma_result = ExecSql(journal_sql);
  if (!pragma_result) {
    auto err = pragma_result.Error();
    sqlite3_close(db_);
    db_ = nullptr;
    return err;
  }

  auto fk_result = ExecSql(std::string(config.sqlite_foreign_keys ? "PRAGMA foreign_keys = ON"
                                                                  : "PRAGMA foreign_keys = OFF"));
  if (!fk_result) {
    auto err = fk_result.Error();
    sqlite3_close(db_);
    db_ = nullptr;
    return err;
  }

  auto sync_result = ExecSql("PRAGMA synchronous = NORMAL");
  if (!sync_result) {
    auto err = sync_result.Error();
    sqlite3_close(db_);
    db_ = nullptr;
    return err;
  }

  return {};
}

auto SqliteDatabase::EnsureSchema() -> Result<void> {
  auto schema_result = ExecSql(
      "CREATE TABLE IF NOT EXISTS entities ("
      "dbid INTEGER PRIMARY KEY AUTOINCREMENT,"
      "type_id INTEGER NOT NULL,"
      "blob BLOB NOT NULL,"
      "identifier TEXT,"
      "password_hash TEXT,"
      "auto_load INTEGER NOT NULL DEFAULT 0,"
      "checked_out INTEGER NOT NULL DEFAULT 0,"
      "checkout_ip INTEGER NOT NULL DEFAULT 0,"
      "checkout_port INTEGER NOT NULL DEFAULT 0,"
      "checkout_app_id INTEGER NOT NULL DEFAULT 0,"
      "checkout_eid INTEGER NOT NULL DEFAULT 0,"
      "checkout_version INTEGER NOT NULL DEFAULT 0,"
      "checkout_instance INTEGER NOT NULL DEFAULT 0,"
      "created_at_ms INTEGER NOT NULL DEFAULT 0,"
      "updated_at_ms INTEGER NOT NULL DEFAULT 0"
      ")");
  if (!schema_result) {
    return schema_result.Error();
  }
  auto columns = ExecSql("SELECT checkout_instance FROM entities LIMIT 0");
  if (!columns) return columns.Error();

  auto created_at_result = ExecSqlIgnoringDuplicateColumn(
      "ALTER TABLE entities ADD COLUMN created_at_ms INTEGER NOT NULL DEFAULT 0");
  if (!created_at_result) {
    return created_at_result.Error();
  }

  auto updated_at_result = ExecSqlIgnoringDuplicateColumn(
      "ALTER TABLE entities ADD COLUMN updated_at_ms INTEGER NOT NULL DEFAULT 0");
  if (!updated_at_result) {
    return updated_at_result.Error();
  }

  auto meta_table_result =
      ExecSql("CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value TEXT NOT NULL)");
  if (!meta_table_result) {
    return meta_table_result.Error();
  }

  auto schema_version_result = UpsertMeta("schema_version", std::to_string(kSqliteSchemaVersion));
  if (!schema_version_result) {
    return schema_version_result.Error();
  }

  auto index1 =
      ExecSql("CREATE INDEX IF NOT EXISTS idx_entities_type_dbid ON entities(type_id, dbid)");
  if (!index1) {
    return index1.Error();
  }

  auto index2 = ExecSql(
      "CREATE INDEX IF NOT EXISTS idx_entities_type_identifier "
      "ON entities(type_id, identifier)");
  if (!index2) {
    return index2.Error();
  }

  auto index3 = ExecSql(
      "CREATE UNIQUE INDEX IF NOT EXISTS idx_entities_type_identifier_unique "
      "ON entities(type_id, identifier) "
      "WHERE identifier IS NOT NULL AND identifier <> ''");
  if (!index3) {
    return index3.Error();
  }

  auto index4 = ExecSql(
      "CREATE INDEX IF NOT EXISTS idx_entities_auto_load ON entities(auto_load, type_id, dbid)");
  if (!index4) {
    return index4.Error();
  }

  auto index5 = ExecSql(
      "CREATE INDEX IF NOT EXISTS idx_entities_checkout_addr "
      "ON entities(checked_out, checkout_ip, checkout_port)");
  if (!index5) {
    return index5.Error();
  }

  auto versions = ExecSql(
      "CREATE TABLE IF NOT EXISTS atlas_checkout_versions ("
      "dbid INTEGER NOT NULL, type_id INTEGER NOT NULL, "
      "version INTEGER NOT NULL CHECK(typeof(version) = 'integer' AND version > 0), "
      "PRIMARY KEY(dbid, type_id))");
  if (!versions) return versions.Error();

  auto processes = ExecSql(
      "CREATE TABLE IF NOT EXISTS atlas_checkout_processes ("
      "base_ip INTEGER NOT NULL, base_port INTEGER NOT NULL, instance INTEGER NOT NULL,"
      "retired INTEGER NOT NULL DEFAULT 0, PRIMARY KEY(base_ip, base_port, instance))");
  if (!processes) return processes.Error();

  auto receipts = ExecSql(
      "CREATE TABLE IF NOT EXISTS atlas_write_receipts ("
      "operation_id BLOB PRIMARY KEY NOT NULL CHECK(length(operation_id) = 16),"
      "request_hash BLOB NOT NULL CHECK(length(request_hash) = 32),"
      "result_dbid INTEGER NOT NULL CHECK(result_dbid > 0),"
      "checkout_version INTEGER NOT NULL CHECK(checkout_version >= 0))");
  if (!receipts) return receipts.Error();

  auto eid_table_result = ExecSql(
      "CREATE TABLE IF NOT EXISTS atlas_entity_id_counter ("
      "id INTEGER PRIMARY KEY CHECK(id = 1),"
      "next_id INTEGER NOT NULL DEFAULT 1"
      ")");
  if (!eid_table_result) {
    return eid_table_result.Error();
  }

  auto begin = ExecSql("BEGIN IMMEDIATE");
  if (!begin) return begin.Error();
  auto seed = ExecSql(
      "INSERT OR IGNORE INTO atlas_entity_id_counter(id, next_id) "
      "SELECT 1, 1 WHERE NOT EXISTS "
      "(SELECT 1 FROM meta WHERE key = 'entity_id_counter_initialized'); "
      "INSERT OR IGNORE INTO meta(key, value) "
      "VALUES('entity_id_counter_initialized', '1')");
  if (!seed) {
    (void)ExecSql("ROLLBACK");
    return seed.Error();
  }
  auto committed = ExecSql("COMMIT");
  if (!committed) {
    (void)ExecSql("ROLLBACK");
    return Error{ErrorCode::kCommitUnknown, std::string(committed.Error().Message())};
  }
  return {};
}

auto SqliteDatabase::ExecSql(std::string_view sql) -> Result<void> {
  char* err = nullptr;
  auto rc = sqlite3_exec(db_, std::string(sql).c_str(), nullptr, nullptr, &err);
  if (rc == SQLITE_OK) {
    return {};
  }

  std::string message = std::format("SqliteDatabase: exec failed '{}'", sql);
  if (err != nullptr) {
    message += ": ";
    message += err;
    sqlite3_free(err);
  } else if (db_ != nullptr) {
    message += ": ";
    message += sqlite3_errmsg(db_);
  }

  return Error{ErrorCode::kInternalError, std::move(message)};
}

auto SqliteDatabase::ExecSqlIgnoringDuplicateColumn(std::string_view sql) -> Result<void> {
  auto result = ExecSql(sql);
  if (result) {
    return result;
  }

  if (std::string_view(result.Error().Message()).find("duplicate column name") !=
      std::string_view::npos) {
    return {};
  }

  return result.Error();
}

auto SqliteDatabase::Prepare(std::string_view sql) -> Result<Statement> {
  sqlite3_stmt* stmt = nullptr;
  auto rc = sqlite3_prepare_v2(db_, std::string(sql).c_str(), -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    return SqliteError(std::format("SqliteDatabase: prepare failed '{}'", sql), rc);
  }
  return Statement{stmt};
}

auto SqliteDatabase::UpsertMeta(std::string_view key, std::string_view value) -> Result<void> {
  auto stmt_result = Prepare("INSERT OR REPLACE INTO meta(key, value) VALUES(?, ?)");
  if (!stmt_result) {
    return stmt_result.Error();
  }

  auto stmt = std::move(*stmt_result);
  auto rc = sqlite3_bind_text(stmt.Get(), 1, std::string(key).c_str(), -1, SQLITE_TRANSIENT);
  if (rc == SQLITE_OK)
    rc = sqlite3_bind_text(stmt.Get(), 2, std::string(value).c_str(), -1, SQLITE_TRANSIENT);
  if (rc != SQLITE_OK) {
    return SqliteError("SqliteDatabase: meta bind failed", rc);
  }

  rc = sqlite3_step(stmt.Get());
  if (rc != SQLITE_DONE) {
    return SqliteError("SqliteDatabase: meta upsert failed", rc);
  }

  return {};
}

auto SqliteDatabase::FetchByDbid(DatabaseID dbid, uint16_t type_id) -> Result<EntityRow> {
  auto stmt_result = Prepare(
      "SELECT dbid, type_id, blob, identifier, password_hash, auto_load, checked_out, "
      "checkout_ip, checkout_port, checkout_app_id, checkout_eid, checkout_version, "
      "checkout_instance "
      "FROM entities WHERE dbid = ? AND type_id = ? LIMIT 1");
  if (!stmt_result) {
    return stmt_result.Error();
  }

  auto stmt = std::move(*stmt_result);
  auto rc = sqlite3_bind_int64(stmt.Get(), 1, dbid);
  if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt.Get(), 2, static_cast<int>(type_id));
  if (rc != SQLITE_OK) {
    return SqliteError("SqliteDatabase: fetch_by_dbid bind failed", rc);
  }

  rc = sqlite3_step(stmt.Get());
  if (rc == SQLITE_DONE) {
    return EntityRow{};
  }
  if (rc != SQLITE_ROW) {
    return SqliteError("SqliteDatabase: fetch_by_dbid step failed", rc);
  }
  return ReadRow(stmt.Get());
}

auto SqliteDatabase::FetchByName(uint16_t type_id, std::string_view identifier)
    -> Result<EntityRow> {
  auto stmt_result = Prepare(
      "SELECT dbid, type_id, blob, identifier, password_hash, auto_load, checked_out, "
      "checkout_ip, checkout_port, checkout_app_id, checkout_eid, checkout_version, "
      "checkout_instance "
      "FROM entities WHERE type_id = ? AND identifier = ? LIMIT 1");
  if (!stmt_result) {
    return stmt_result.Error();
  }

  auto stmt = std::move(*stmt_result);
  auto rc = sqlite3_bind_int(stmt.Get(), 1, static_cast<int>(type_id));
  auto owned_identifier = std::string(identifier);
  if (rc == SQLITE_OK)
    rc = sqlite3_bind_text(stmt.Get(), 2, owned_identifier.c_str(), -1, SQLITE_TRANSIENT);
  if (rc != SQLITE_OK) {
    return SqliteError("SqliteDatabase: fetch_by_name bind failed", rc);
  }

  rc = sqlite3_step(stmt.Get());
  if (rc == SQLITE_DONE) {
    return EntityRow{};
  }
  if (rc != SQLITE_ROW) {
    return SqliteError("SqliteDatabase: fetch_by_name step failed", rc);
  }
  return ReadRow(stmt.Get());
}

auto SqliteDatabase::ReadRow(sqlite3_stmt* stmt) const -> Result<EntityRow> {
  EntityRow row;
  row.found = true;
  row.data.dbid = static_cast<DatabaseID>(sqlite3_column_int64(stmt, 0));
  row.data.type_id = static_cast<uint16_t>(sqlite3_column_int(stmt, 1));

  auto blob_ptr = sqlite3_column_blob(stmt, 2);
  auto blob_size = sqlite3_column_bytes(stmt, 2);
  if (blob_ptr != nullptr && blob_size > 0) {
    auto* bytes = static_cast<const std::byte*>(blob_ptr);
    row.data.blob.assign(bytes, bytes + blob_size);
  }

  if (const auto* text = sqlite3_column_text(stmt, 3); text != nullptr) {
    row.data.identifier = reinterpret_cast<const char*>(text);
  }
  if (const auto* text = sqlite3_column_text(stmt, 4); text != nullptr) {
    row.password_hash = reinterpret_cast<const char*>(text);
  }

  row.auto_load = sqlite3_column_int(stmt, 5) != 0;
  const bool kCheckedOut = sqlite3_column_int(stmt, 6) != 0;
  const auto version = sqlite3_column_int64(stmt, 11);
  if (sqlite3_column_type(stmt, 11) != SQLITE_INTEGER || version < 0 ||
      (kCheckedOut && version == 0)) {
    return Error{ErrorCode::kIoError, "invalid stored checkout version"};
  }
  if (kCheckedOut) {
    CheckoutInfo info;
    info.base_addr = Address(static_cast<uint32_t>(sqlite3_column_int64(stmt, 7)),
                             static_cast<uint16_t>(sqlite3_column_int(stmt, 8)));
    info.app_id = static_cast<uint32_t>(sqlite3_column_int(stmt, 9));
    info.entity_id = static_cast<uint32_t>(sqlite3_column_int(stmt, 10));
    info.ownership_version = static_cast<uint64_t>(version);
    info.process_instance = static_cast<uint64_t>(sqlite3_column_int64(stmt, 12));
    row.checked_out_by = info;
  }

  return row;
}

auto SqliteDatabase::BindIdentifier(Statement& stmt, int index, const std::string& identifier) const
    -> Result<void> {
  int rc = SQLITE_OK;
  if (identifier.empty()) {
    rc = sqlite3_bind_null(stmt.Get(), index);
  } else {
    rc = sqlite3_bind_text(stmt.Get(), index, identifier.c_str(), -1, SQLITE_TRANSIENT);
  }
  if (rc != SQLITE_OK) {
    return SqliteError("SqliteDatabase: bind identifier failed", rc);
  }
  return {};
}

auto SqliteDatabase::BindBlob(Statement& stmt, int index, std::span<const std::byte> blob) const
    -> Result<void> {
  static const std::byte kEmptyBlob = std::byte{0};
  const void* data = blob.empty() ? &kEmptyBlob : blob.data();
  auto rc =
      sqlite3_bind_blob(stmt.Get(), index, data, static_cast<int>(blob.size()), SQLITE_TRANSIENT);
  if (rc != SQLITE_OK) {
    return SqliteError("SqliteDatabase: bind blob failed", rc);
  }
  return {};
}

auto SqliteDatabase::SqliteError(std::string_view prefix) const -> Error {
  std::string message(prefix);
  if (db_ != nullptr) {
    message += ": ";
    message += sqlite3_errmsg(db_);
  }
  return Error{ErrorCode::kInternalError, std::move(message)};
}

auto SqliteDatabase::SqliteError(std::string_view prefix, int code) const -> Error {
  return Error{ErrorCode::kInternalError,
               std::format("{} (sqlite rc={}){}", prefix, code,
                           db_ != nullptr ? std::format(": {}", sqlite3_errmsg(db_)) : "")};
}

}  // namespace atlas
