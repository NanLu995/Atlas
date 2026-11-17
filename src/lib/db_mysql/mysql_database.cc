#include "db_mysql/mysql_database.h"

#include <mysql.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <utility>

#include "db/entity_blob.h"
#include "db_mysql/mysql_client.h"
#include "foundation/log.h"

namespace atlas {

namespace {
constexpr int kMysqlSchemaVersion = 4;
constexpr unsigned int kErDupEntry = 1062;  // MySQL ER_DUP_ENTRY

auto UnixTimeMs() -> int64_t {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}
}  // namespace

MysqlDatabase::~MysqlDatabase() {
  if (started_) {
    Shutdown();
  }
}

// ── lifecycle ───────────────────────────────────────────────────────────────

auto MysqlDatabase::Startup(const DatabaseConfig& config, const EntityDefRegistry& entity_defs)
    -> Result<void> {
  if (started_) return Error{ErrorCode::kAlreadyExists, "MysqlDatabase is already started"};
  if (config.mysql_host.empty() || config.mysql_user.empty() || config.mysql_password.empty() ||
      config.mysql_database.empty() || config.mysql_port == 0 || config.mysql_pool_size <= 0) {
    return Error{ErrorCode::kInvalidArgument,
                 "MySQL requires host, port, user, password, database and a positive pool size; "
                 "configure MySQL explicitly; SQLite fallback is disabled"};
  }
  if (config.mysql_database_id.size() != 32 ||
      config.mysql_database_id.find_first_not_of("0123456789abcdef") != std::string::npos) {
    return Error{ErrorCode::kInvalidArgument,
                 "MySQL mysql_database_id requires 32 lowercase hexadecimal characters; "
                 "configure the expected logical database identity explicitly"};
  }
  if (config.mysql_ssl_ca.empty()) {
    return Error{ErrorCode::kInvalidArgument,
                 "MySQL requires mysql_ssl_ca; TLS certificate and target identity verification "
                 "cannot be disabled"};
  }
  stopping_ = false;
  entity_defs_ = &entity_defs;
  config_ = config;
  auto storage_schema = entity_defs.StorageSchemaDigest();
  if (!storage_schema) return storage_schema.Error();
  storage_schema_ = *storage_schema;

  conn_ = OpenConnection();
  if (conn_ == nullptr) {
    return Error{ErrorCode::kIoError,
                 std::format("MysqlDatabase: cannot connect to {}:{}/{}", config.mysql_host,
                             config.mysql_port, config.mysql_database)};
  }

  auto schema = EnsureSchema(conn_, *storage_schema);
  if (!schema) {
    mysql_close(conn_);
    conn_ = nullptr;
    return schema.Error();
  }

  const int n = std::max(1, config.mysql_pool_size);
  for (int i = 0; i < n; ++i) {
    workers_.push_back(std::make_unique<Worker>());
  }
  for (auto& w : workers_) {
    Worker* wp = w.get();
    wp->thread = std::thread([this, wp] { WorkerLoop(*wp); });
  }

  started_ = true;
  ATLAS_LOG_INFO("MysqlDatabase: connected to {}:{}/{} ({} workers)", config.mysql_host,
                 config.mysql_port, config.mysql_database, n);
  return {};
}

void MysqlDatabase::BeginDrain() {
  stopping_ = true;
  for (auto& w : workers_) {
    {
      std::lock_guard<std::mutex> lock(w->mutex);
      w->stop = true;
    }
    w->cv.notify_all();
  }
}

void MysqlDatabase::Shutdown() {
  BeginDrain();
  for (auto& w : workers_) {
    if (w->thread.joinable()) w->thread.join();
  }
  workers_.clear();

  for (;;) {
    ProcessResults();
    std::lock_guard<std::mutex> lock(results_mutex_);
    if (results_.empty()) break;
  }

  if (conn_ != nullptr) {
    mysql_close(conn_);
    conn_ = nullptr;
  }
  started_ = false;
}

auto MysqlDatabase::AdmitOperation() const -> Result<void> {
  if (stopping_) return Error{ErrorCode::kReceiverGone, "database is stopping"};
  if (!started_) return Error{ErrorCode::kIoError, "database is not started"};
  if (deferred_mode_ && PendingOperations() >= kMaxPendingOperations)
    return Error{ErrorCode::kRateLimited, "database operation capacity reached"};
  return {};
}

auto MysqlDatabase::OpenConnection() const -> MYSQL* {
  if (!InitializeMysqlThread()) {
    ATLAS_LOG_ERROR("MysqlDatabase: Oracle client thread initialization failed");
    return nullptr;
  }
  MYSQL* conn = mysql_init(nullptr);
  if (conn == nullptr) return nullptr;

  unsigned int protocol = MYSQL_PROTOCOL_TCP;
  unsigned int connect_timeout = 10;
  unsigned int query_timeout = 5;
  unsigned int local_infile = 0;
  enum mysql_ssl_mode ssl_mode = SSL_MODE_VERIFY_IDENTITY;
  // Reconnecting within a transaction could turn a rolled-back COMMIT into success.
  bool reconnect = false;
  if (mysql_options(conn, MYSQL_OPT_PROTOCOL, &protocol) != 0 ||
      mysql_options(conn, MYSQL_OPT_CONNECT_TIMEOUT, &connect_timeout) != 0 ||
      mysql_options(conn, MYSQL_SET_CHARSET_NAME, "utf8mb4") != 0 ||
      mysql_options(conn, MYSQL_OPT_RECONNECT, &reconnect) != 0 ||
      mysql_options(conn, MYSQL_OPT_READ_TIMEOUT, &query_timeout) != 0 ||
      mysql_options(conn, MYSQL_OPT_WRITE_TIMEOUT, &query_timeout) != 0 ||
      mysql_options(conn, MYSQL_OPT_LOCAL_INFILE, &local_infile) != 0 ||
      mysql_options(conn, MYSQL_OPT_SSL_CA, config_.mysql_ssl_ca.c_str()) != 0 ||
      mysql_options(conn, MYSQL_OPT_SSL_MODE, &ssl_mode) != 0 ||
      mysql_options(conn, MYSQL_OPT_TLS_VERSION, "TLSv1.2,TLSv1.3") != 0) {
    ATLAS_LOG_ERROR("MysqlDatabase: Oracle client options rejected: {}", mysql_error(conn));
    mysql_close(conn);
    return nullptr;
  }

  if (mysql_real_connect(conn, config_.mysql_host.c_str(), config_.mysql_user.c_str(),
                         config_.mysql_password.c_str(), config_.mysql_database.c_str(),
                         config_.mysql_port, nullptr, 0) == nullptr) {
    ATLAS_LOG_ERROR("MysqlDatabase: connect failed: {}", mysql_error(conn));
    mysql_close(conn);
    return nullptr;
  }
  const auto version = mysql_get_server_version(conn);
  if (version < 80400 || version >= 80500 ||
      std::string_view(mysql_get_server_info(conn)).find("MariaDB") != std::string_view::npos) {
    ATLAS_LOG_ERROR("MysqlDatabase: Oracle MySQL 8.4 LTS server required");
    mysql_close(conn);
    return nullptr;
  }
  return conn;
}

auto MysqlDatabase::EnsureSchema(MYSQL* conn, const Sha256Digest& storage_schema) const
    -> Result<void> {
  constexpr std::string_view kLock = "CONCAT('atlas.schema.', LEFT(SHA2(DATABASE(), 256), 48))";
  // Keep lock acquisition within its own budget instead of relying on I/O timeouts.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  for (;;) {
    auto locked = QueryScalar(conn, std::format("SELECT GET_LOCK({}, 0)", kLock));
    if (!locked) return locked.Error();
    if (*locked == "1") break;
    if (*locked != "0" || std::chrono::steady_clock::now() >= deadline)
      return Error{ErrorCode::kTimeout, "MysqlDatabase: schema initialization lock unavailable"};
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  auto schema = EnsureSchemaLocked(conn, storage_schema);
  auto released = QueryScalar(conn, std::format("SELECT RELEASE_LOCK({})", kLock));
  if (!schema) return schema.Error();
  if (!released) return released.Error();
  if (*released != "1")
    return Error{ErrorCode::kInternalError, "MysqlDatabase: schema initialization lock lost"};
  return {};
}

auto MysqlDatabase::ValidateSchema(MYSQL* conn, const Sha256Digest& storage_schema) const
    -> Result<void> {
  auto version = QueryScalar(conn, "SELECT `value` FROM meta WHERE `key` = 'schema_version'");
  if (!version)
    return Error{
        version.Error().Code(),
        std::format("MysqlDatabase: cannot read schema_version: {}", version.Error().Message())};
  if (*version != std::to_string(kMysqlSchemaVersion))
    return Error{ErrorCode::kInvalidArgument,
                 "MysqlDatabase: missing or incompatible schema_version; use a new empty "
                 "database with matching server binaries; automatic upgrade is disabled"};
  auto identity = QueryScalar(conn, "SELECT `value` FROM meta WHERE `key`='logical_database_id'");
  if (!identity) return identity.Error();
  if (*identity != config_.mysql_database_id)
    return Error{ErrorCode::kPermissionDenied,
                 "MysqlDatabase: missing or mismatched logical_database_id; database rejected"};
  auto format = QueryScalar(conn, "SELECT `value` FROM meta WHERE `key` = 'blob_format_version'");
  if (!format) return format.Error();
  if (*format != std::to_string(kDbBlobVersion))
    return Error{ErrorCode::kInvalidArgument,
                 "MysqlDatabase: missing or incompatible blob_format_version"};
  auto digest = QueryScalar(conn, "SELECT `value` FROM meta WHERE `key` = 'storage_schema_digest'");
  if (!digest) return digest.Error();
  if (!digest->has_value() ||
      **digest != std::string_view(reinterpret_cast<const char*>(storage_schema.data()),
                                   storage_schema.size()))
    return Error{ErrorCode::kInvalidArgument,
                 "MysqlDatabase: missing or incompatible storage_schema_digest; "
                 "database and entity definitions must have the same snapshot schema"};
  return ExecSql(conn, "SELECT checkout_instance FROM entities LIMIT 0");
}

auto MysqlDatabase::EnsureSchemaLocked(MYSQL* conn, const Sha256Digest& storage_schema) const
    -> Result<void> {
  auto existing = QueryScalar(conn,
                              "SELECT TABLE_NAME FROM information_schema.TABLES "
                              "WHERE TABLE_SCHEMA = DATABASE() LIMIT 1");
  if (!existing) return existing.Error();
  if (existing->has_value()) return ValidateSchema(conn, storage_schema);
  if (!config_.mysql_initialize)
    return Error{ErrorCode::kInvalidArgument,
                 "MysqlDatabase: empty database rejected; initial creation requires explicit "
                 "mysql_initialize=true; do not enable it during recovery or endpoint switching"};

  auto entities = ExecSql(conn,
                          "CREATE TABLE IF NOT EXISTS entities ("
                          "dbid BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,"
                          "type_id INT UNSIGNED NOT NULL,"
                          "`blob` LONGBLOB NOT NULL,"
                          "identifier VARBINARY(255) NULL,"
                          "password_hash VARBINARY(255) NULL,"
                          "auto_load TINYINT NOT NULL DEFAULT 0,"
                          "checked_out TINYINT NOT NULL DEFAULT 0,"
                          "checkout_ip INT UNSIGNED NOT NULL DEFAULT 0,"
                          "checkout_port INT UNSIGNED NOT NULL DEFAULT 0,"
                          "checkout_app_id INT UNSIGNED NOT NULL DEFAULT 0,"
                          "checkout_eid INT UNSIGNED NOT NULL DEFAULT 0,"
                          "checkout_version BIGINT UNSIGNED NOT NULL DEFAULT 0,"
                          "checkout_instance BIGINT UNSIGNED NOT NULL DEFAULT 0,"
                          "created_at_ms BIGINT NOT NULL DEFAULT 0,"
                          "updated_at_ms BIGINT NOT NULL DEFAULT 0,"
                          "PRIMARY KEY (dbid),"
                          "UNIQUE KEY uk_entities_type_identifier (type_id, identifier),"
                          "KEY idx_entities_type_dbid (type_id, dbid),"
                          "KEY idx_entities_auto_load (auto_load, type_id, dbid),"
                          "KEY idx_entities_checkout_addr (checked_out, checkout_ip, checkout_port)"
                          ") ENGINE=InnoDB DEFAULT CHARSET=binary");
  if (!entities) return entities.Error();
  auto columns = ExecSql(conn, "SELECT checkout_instance FROM entities LIMIT 0");
  if (!columns) return columns.Error();

  auto meta = ExecSql(conn,
                      "CREATE TABLE IF NOT EXISTS meta ("
                      "`key` VARBINARY(64) NOT NULL,"
                      "`value` VARBINARY(255) NOT NULL,"
                      "PRIMARY KEY (`key`)"
                      ") ENGINE=InnoDB DEFAULT CHARSET=binary");
  if (!meta) return meta.Error();

  auto counter = ExecSql(conn,
                         "CREATE TABLE IF NOT EXISTS atlas_entity_id_counter ("
                         "id INT NOT NULL,"
                         "next_id BIGINT UNSIGNED NOT NULL DEFAULT 1,"
                         "PRIMARY KEY (id),"
                         "CHECK (id = 1)"
                         ") ENGINE=InnoDB DEFAULT CHARSET=binary");
  if (!counter) return counter.Error();

  auto versions = ExecSql(conn,
                          "CREATE TABLE IF NOT EXISTS atlas_checkout_versions ("
                          "dbid BIGINT UNSIGNED NOT NULL, type_id INT UNSIGNED NOT NULL,"
                          "version BIGINT UNSIGNED NOT NULL, PRIMARY KEY (dbid, type_id)"
                          ") ENGINE=InnoDB DEFAULT CHARSET=binary");
  if (!versions) return versions.Error();

  auto processes =
      ExecSql(conn,
              "CREATE TABLE IF NOT EXISTS atlas_checkout_processes ("
              "base_ip INT UNSIGNED NOT NULL, base_port INT UNSIGNED NOT NULL,"
              "instance BIGINT UNSIGNED NOT NULL, retired TINYINT NOT NULL DEFAULT 0,"
              "PRIMARY KEY(base_ip, base_port, instance)) ENGINE=InnoDB DEFAULT CHARSET=binary");
  if (!processes) return processes.Error();

  auto receipts = ExecSql(conn,
                          "CREATE TABLE IF NOT EXISTS atlas_write_receipts ("
                          "operation_id BINARY(16) NOT NULL PRIMARY KEY,"
                          "request_hash BINARY(32) NOT NULL,"
                          "result_dbid BIGINT UNSIGNED NULL,"
                          "result_checkout_version BIGINT UNSIGNED NOT NULL DEFAULT 0"
                          ") ENGINE=InnoDB DEFAULT CHARSET=binary");
  if (!receipts) return receipts.Error();

  auto sessions = ExecSql(conn,
                          "CREATE TABLE IF NOT EXISTS atlas_account_sessions ("
                          "dbid BIGINT UNSIGNED NOT NULL, type_id INT UNSIGNED NOT NULL,"
                          "version BIGINT UNSIGNED NOT NULL, payload VARBINARY(256) NOT NULL,"
                          "PRIMARY KEY(dbid, type_id)) ENGINE=InnoDB DEFAULT CHARSET=binary");
  if (!sessions) return sessions.Error();

  auto begin = BeginTxn(conn);
  if (!begin) return begin.Error();
  auto seed = ExecSql(conn,
                      "INSERT IGNORE INTO atlas_entity_id_counter(id, next_id) "
                      "SELECT 1, 1 WHERE NOT EXISTS "
                      "(SELECT 1 FROM meta WHERE `key` = 'entity_id_counter_initialized')");
  if (seed) {
    seed = ExecSql(conn, std::format("INSERT INTO meta(`key`,`value`) "
                                     "VALUES('logical_database_id',{})",
                                     IdentifierLiteral(config_.mysql_database_id)));
  }
  if (seed) {
    seed = ExecSql(conn, std::format("INSERT INTO meta(`key`,`value`) "
                                     "VALUES('storage_schema_digest',{})",
                                     HexLiteral(storage_schema)));
  }
  if (seed) {
    seed = ExecSql(conn,
                   "INSERT IGNORE INTO meta(`key`, `value`) "
                   "VALUES('entity_id_counter_initialized', '1')");
  }
  if (seed) {
    seed = ExecSql(conn, std::format("INSERT INTO meta(`key`,`value`) "
                                     "VALUES('blob_format_version','{}')",
                                     kDbBlobVersion));
  }
  if (seed) {
    seed = ExecSql(conn, std::format("INSERT INTO meta(`key`,`value`) "
                                     "VALUES('schema_version','{}')",
                                     kMysqlSchemaVersion));
  }
  if (!seed) {
    Rollback(conn);
    return seed.Error();
  }
  auto committed = Commit(conn);
  if (!committed) Rollback(conn);
  return committed;
}

// ── worker pool ─────────────────────────────────────────────────────────────

void MysqlDatabase::WorkerLoop(Worker& worker) {
  MYSQL* conn = nullptr;
  for (;;) {
    std::function<void(MYSQL*)> job;
    {
      std::unique_lock<std::mutex> lock(worker.mutex);
      worker.cv.wait(lock, [&] { return worker.stop || !worker.jobs.empty(); });
      if (worker.stop && worker.jobs.empty()) break;
      job = std::move(worker.jobs.front());
      worker.jobs.pop_front();
    }
    RefreshConnection(conn);
    job(conn);
  }

  if (conn != nullptr) mysql_close(conn);
}

void MysqlDatabase::RefreshConnection(MYSQL*& conn) const {
  if (conn == nullptr || mysql_errno(conn) != 0) {
    if (conn != nullptr) mysql_close(conn);
    conn = OpenConnection();
    if (conn != nullptr) {
      auto validated = ValidateSchema(conn, storage_schema_);
      if (!validated) {
        ATLAS_LOG_ERROR("MysqlDatabase: reconnected storage rejected: {}",
                        validated.Error().Message());
        mysql_close(conn);
        conn = nullptr;
      }
    }
  }
}

void MysqlDatabase::EnqueueForEntity(uint16_t type_id, uint64_t key,
                                     std::function<void(MYSQL*)> job) {
  const std::size_t idx =
      static_cast<std::size_t>(key * 1099511628211ULL + type_id) % workers_.size();
  Worker& w = *workers_[idx];
  {
    std::lock_guard<std::mutex> lock(w.mutex);
    w.jobs.push_back(std::move(job));
  }
  w.cv.notify_one();
}

void MysqlDatabase::PostResult(std::function<void()> result) {
  std::lock_guard<std::mutex> lock(results_mutex_);
  results_.push_back(std::move(result));
}

void MysqlDatabase::ProcessResults() {
  for (int fired = 0; fired < kMaxCallbacksPerTick; ++fired) {
    std::function<void()> result;
    {
      std::lock_guard<std::mutex> lock(results_mutex_);
      if (results_.empty()) return;
      result = std::move(results_.front());
      results_.pop_front();
    }
    result();
  }
}

// ── SQL helpers (operate on a caller-supplied connection) ───────────────────

auto MysqlDatabase::ExecSql(MYSQL* conn, std::string_view sql) -> Result<void> {
  if (conn == nullptr) {
    return Error{ErrorCode::kIoError, "MysqlDatabase: not connected"};
  }
  if (mysql_real_query(conn, sql.data(), static_cast<unsigned long>(sql.size())) != 0) {
    return MysqlErr(conn, "MysqlDatabase: query failed");
  }
  MYSQL_RES* res = mysql_store_result(conn);
  if (res != nullptr) {
    mysql_free_result(res);
  }
  return {};
}

auto MysqlDatabase::QueryScalar(MYSQL* conn, std::string_view sql)
    -> Result<std::optional<std::string>> {
  if (mysql_real_query(conn, sql.data(), static_cast<unsigned long>(sql.size())) != 0)
    return MysqlErr(conn, "MysqlDatabase: schema query failed");
  std::unique_ptr<MYSQL_RES, decltype(&mysql_free_result)> rows(mysql_store_result(conn),
                                                                mysql_free_result);
  if (!rows) return MysqlErr(conn, "MysqlDatabase: schema query result unavailable");
  auto row = mysql_fetch_row(rows.get());
  if (!row || !row[0]) return std::optional<std::string>{};
  return std::optional<std::string>{std::string(row[0], mysql_fetch_lengths(rows.get())[0])};
}

auto MysqlDatabase::QueryRow(MYSQL* conn, std::string_view sql) -> Result<EntityRow> {
  if (conn == nullptr) {
    return Error{ErrorCode::kIoError, "MysqlDatabase: not connected"};
  }
  if (mysql_real_query(conn, sql.data(), static_cast<unsigned long>(sql.size())) != 0) {
    return MysqlErr(conn, "MysqlDatabase: select failed");
  }
  MYSQL_RES* res = mysql_store_result(conn);
  if (res == nullptr) {
    if (mysql_errno(conn) != 0) return MysqlErr(conn, "MysqlDatabase: store_result failed");
    return EntityRow{};
  }
  MYSQL_ROW row = mysql_fetch_row(res);
  if (row == nullptr) {
    mysql_free_result(res);
    return EntityRow{};
  }
  auto out = ReadRow(row, mysql_fetch_lengths(res));
  mysql_free_result(res);
  return out;
}

auto MysqlDatabase::ReadRow(char** row, const unsigned long* len) -> EntityRow {
  EntityRow r;
  r.found = true;
  r.data.dbid = static_cast<DatabaseID>(std::strtoull(row[0], nullptr, 10));
  r.data.type_id = static_cast<uint16_t>(std::strtoul(row[1], nullptr, 10));
  if (row[2] != nullptr && len[2] > 0) {
    const auto* bytes = reinterpret_cast<const std::byte*>(row[2]);
    r.data.blob.assign(bytes, bytes + len[2]);
  }
  if (row[3] != nullptr) r.data.identifier.assign(row[3], len[3]);
  if (row[4] != nullptr) r.password_hash.assign(row[4], len[4]);
  r.auto_load = row[5] != nullptr && std::strtol(row[5], nullptr, 10) != 0;
  const bool checked_out = row[6] != nullptr && std::strtol(row[6], nullptr, 10) != 0;
  r.checkout_version = std::strtoull(row[11], nullptr, 10);
  if (checked_out) {
    CheckoutInfo info;
    info.ownership_version = r.checkout_version;
    info.process_instance = std::strtoull(row[12], nullptr, 10);
    info.base_addr = Address(static_cast<uint32_t>(std::strtoul(row[7], nullptr, 10)),
                             static_cast<uint16_t>(std::strtoul(row[8], nullptr, 10)));
    info.app_id = static_cast<uint32_t>(std::strtoul(row[9], nullptr, 10));
    info.entity_id = static_cast<uint32_t>(std::strtoul(row[10], nullptr, 10));
    r.checked_out_by = info;
  }
  return r;
}

auto MysqlDatabase::FetchByDbid(MYSQL* conn, DatabaseID dbid, uint16_t type_id, bool for_update)
    -> Result<EntityRow> {
  return QueryRow(
      conn, std::format("SELECT dbid, type_id, `blob`, identifier, password_hash, auto_load, "
                        "checked_out, checkout_ip, checkout_port, checkout_app_id, checkout_eid, "
                        "checkout_version, checkout_instance "
                        "FROM entities WHERE dbid = {} AND type_id = {} LIMIT 1 {}",
                        static_cast<uint64_t>(dbid), type_id, for_update ? "FOR UPDATE" : ""));
}

auto MysqlDatabase::FetchByName(MYSQL* conn, uint16_t type_id, std::string_view identifier,
                                bool for_update) -> Result<EntityRow> {
  return QueryRow(
      conn, std::format("SELECT dbid, type_id, `blob`, identifier, password_hash, auto_load, "
                        "checked_out, checkout_ip, checkout_port, checkout_app_id, checkout_eid, "
                        "checkout_version, checkout_instance "
                        "FROM entities WHERE type_id = {} AND identifier = {} LIMIT 1 {}",
                        type_id, IdentifierLiteral(identifier), for_update ? "FOR UPDATE" : ""));
}

auto MysqlDatabase::HexLiteral(std::span<const std::byte> data) -> std::string {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(data.size() * 2 + 3);
  out += "X'";
  for (auto b : data) {
    const auto v = static_cast<unsigned char>(b);
    out += kHex[v >> 4];
    out += kHex[v & 0x0F];
  }
  out += '\'';
  return out;
}

auto MysqlDatabase::IdentifierLiteral(std::string_view identifier) -> std::string {
  if (identifier.empty()) return "NULL";
  return HexLiteral(std::as_bytes(std::span<const char>(identifier.data(), identifier.size())));
}

auto MysqlDatabase::MysqlErr(MYSQL* conn, std::string_view prefix) -> Error {
  const char* msg = (conn != nullptr) ? mysql_error(conn) : "no connection";
  return Error{ErrorCode::kIoError, std::format("{}: {}", prefix, msg)};
}

auto MysqlDatabase::ConstraintErrorKind(MYSQL* conn) -> DatabaseErrorKind {
  if (conn == nullptr || mysql_errno(conn) != kErDupEntry) {
    return DatabaseErrorKind::kNone;
  }
  const std::string_view msg = mysql_error(conn);
  return msg.find("PRIMARY") != std::string_view::npos ? DatabaseErrorKind::kDuplicateDbid
                                                       : DatabaseErrorKind::kDuplicateIdentifier;
}

auto MysqlDatabase::BeginTxn(MYSQL* conn) -> Result<void> {
  return ExecSql(conn, "START TRANSACTION");
}

auto MysqlDatabase::Commit(MYSQL* conn) -> Result<void> {
  auto result = ExecSql(conn, "COMMIT");
  if (!result) return Error{ErrorCode::kCommitUnknown, std::string(result.Error().Message())};
  return {};
}

void MysqlDatabase::Rollback(MYSQL* conn) {
  (void)ExecSql(conn, "ROLLBACK");
}

// ── operation bodies ────────────────────────────────────────────────────────

auto MysqlDatabase::DoPut(MYSQL* conn, DatabaseID dbid, uint16_t type_id, WriteFlags flags,
                          std::span<const std::byte> blob, const std::string& identifier,
                          bool write_password, const std::string& password_hash,
                          bool transaction_open) -> PutResult {
  PutResult result;
  if (HasFlag(flags, WriteFlags::kDelete)) {
    auto del = DoDel(conn, dbid, type_id, transaction_open);
    result.success = del.success;
    result.dbid = dbid;
    result.error = std::move(del.error);
    result.error_kind = del.error_kind;
    return result;
  }

  const std::string password_literal = write_password ? IdentifierLiteral(password_hash) : "NULL";
  const auto now = UnixTimeMs();

  if (HasFlag(flags, WriteFlags::kCreateNew) || dbid == kInvalidDBID) {
    const bool explicit_dbid = HasFlag(flags, WriteFlags::kExplicitDbid) && dbid != kInvalidDBID;
    const int auto_load = HasFlag(flags, WriteFlags::kAutoLoadOn) ? 1 : 0;
    std::string sql;
    if (explicit_dbid) {
      sql = std::format(
          "INSERT INTO entities (dbid, type_id, `blob`, identifier, password_hash, auto_load, "
          "checked_out, checkout_ip, checkout_port, checkout_app_id, checkout_eid, created_at_ms, "
          "updated_at_ms) VALUES ({}, {}, {}, {}, {}, {}, 0, 0, 0, 0, 0, {}, {})",
          static_cast<uint64_t>(dbid), type_id, HexLiteral(blob), IdentifierLiteral(identifier),
          password_literal, auto_load, now, now);
    } else {
      sql = std::format(
          "INSERT INTO entities (type_id, `blob`, identifier, password_hash, auto_load, "
          "checked_out, checkout_ip, checkout_port, checkout_app_id, checkout_eid, created_at_ms, "
          "updated_at_ms) VALUES ({}, {}, {}, {}, {}, 0, 0, 0, 0, 0, {}, {})",
          type_id, HexLiteral(blob), IdentifierLiteral(identifier), password_literal, auto_load,
          now, now);
    }
    auto exec = ExecSql(conn, sql);
    if (!exec) {
      result.error_kind = ConstraintErrorKind(conn);
      result.error = std::string(exec.Error().Message());
      return result;
    }
    result.success = true;
    result.dbid = explicit_dbid ? dbid : static_cast<DatabaseID>(mysql_insert_id(conn));
    return result;
  }

  auto row_result = FetchByDbid(conn, dbid, type_id);
  if (!row_result) {
    result.error = std::string(row_result.Error().Message());
    return result;
  }
  auto row = std::move(*row_result);
  if (!row.found) {
    result.error_kind = DatabaseErrorKind::kNotFound;
    result.error = std::format("entity ({},{}) not found", type_id, dbid);
    return result;
  }

  const std::string final_identifier = identifier.empty() ? row.data.identifier : identifier;
  const bool final_auto_load =
      HasFlag(flags, WriteFlags::kAutoLoadOn)
          ? true
          : (HasFlag(flags, WriteFlags::kAutoLoadOff) ? false : row.auto_load);
  const bool final_checked_out =
      HasFlag(flags, WriteFlags::kLogOff) ? false : row.checked_out_by.has_value();
  const CheckoutInfo owner = row.checked_out_by.value_or(CheckoutInfo{});
  const std::string password_set =
      write_password ? std::format("password_hash = {}, ", password_literal) : "";

  auto exec = ExecSql(
      conn,
      std::format(
          "UPDATE entities SET `blob` = {}, identifier = {}, {}auto_load = {}, checked_out = "
          "{}, checkout_ip = {}, checkout_port = {}, checkout_app_id = {}, checkout_eid = {}, "
          "updated_at_ms = {} WHERE dbid = {} AND type_id = {}",
          HexLiteral(blob), IdentifierLiteral(final_identifier), password_set,
          final_auto_load ? 1 : 0, final_checked_out ? 1 : 0,
          final_checked_out ? owner.base_addr.Ip() : 0u,
          final_checked_out ? owner.base_addr.Port() : uint16_t{0},
          final_checked_out ? owner.app_id : 0u, final_checked_out ? owner.entity_id : 0u, now,
          static_cast<uint64_t>(dbid), type_id));
  if (!exec) {
    result.error = std::string(exec.Error().Message());
    return result;
  }
  result.success = true;
  result.dbid = dbid;
  return result;
}

auto MysqlDatabase::LockOperationReceipt(MYSQL* conn, const std::array<std::byte, 16>& id,
                                         const Sha256Digest& hash)
    -> Result<std::optional<DatabaseOperationReceipt>> {
  const auto key = HexLiteral(id);
  const auto digest = HexLiteral(hash);
  auto inserted = ExecSql(
      conn, std::format("INSERT INTO atlas_write_receipts(operation_id, request_hash) "
                        "VALUES({}, {}) ON DUPLICATE KEY UPDATE operation_id = operation_id",
                        key, digest));
  if (!inserted) return inserted.Error();
  const bool created = mysql_affected_rows(conn) == 1;
  const auto query = std::format(
      "SELECT request_hash = {}, result_dbid, result_checkout_version FROM atlas_write_receipts "
      "WHERE operation_id = {} FOR UPDATE",
      digest, key);
  if (mysql_real_query(conn, query.data(), static_cast<unsigned long>(query.size())) != 0)
    return MysqlErr(conn, "operation receipt query failed");
  std::unique_ptr<MYSQL_RES, decltype(&mysql_free_result)> rows(mysql_store_result(conn),
                                                                mysql_free_result);
  MYSQL_ROW row = rows ? mysql_fetch_row(rows.get()) : nullptr;
  if (row == nullptr || row[0] == nullptr || row[2] == nullptr)
    return Error{ErrorCode::kIoError, "operation receipt is missing or unreadable"};
  if (std::string_view(row[0]) != "1")
    return Error{ErrorCode::kAlreadyExists, "operation_id_reused"};
  const std::string_view version(row[2]);
  DatabaseOperationReceipt receipt;
  receipt.request_hash = hash;
  const auto parsed =
      std::from_chars(version.data(), version.data() + version.size(), receipt.checkout_version);
  if (parsed.ec != std::errc{} || parsed.ptr != version.data() + version.size())
    return Error{ErrorCode::kIoError, "invalid operation receipt version"};
  if (row[1] == nullptr) {
    if (created && receipt.checkout_version == 0) return std::optional<DatabaseOperationReceipt>{};
    return Error{ErrorCode::kIoError, "incomplete operation receipt"};
  }
  const std::string_view dbid(row[1]);
  const auto parsed_dbid = std::from_chars(dbid.data(), dbid.data() + dbid.size(), receipt.dbid);
  if (parsed_dbid.ec != std::errc{} || parsed_dbid.ptr != dbid.data() + dbid.size() ||
      receipt.dbid <= kInvalidDBID)
    return Error{ErrorCode::kIoError, "invalid operation receipt DBID"};
  return std::optional{receipt};
}

auto MysqlDatabase::SaveOperationReceipt(MYSQL* conn, const std::array<std::byte, 16>& id,
                                         const DatabaseOperationReceipt& receipt) -> Result<void> {
  auto updated = ExecSql(conn, std::format("UPDATE atlas_write_receipts SET result_dbid = {}, "
                                           "result_checkout_version = {} WHERE operation_id = {} "
                                           "AND result_dbid IS NULL",
                                           receipt.dbid, receipt.checkout_version, HexLiteral(id)));
  if (!updated) return updated.Error();
  if (mysql_affected_rows(conn) != 1)
    return Error{ErrorCode::kIoError, "operation receipt was not recorded"};
  return {};
}

auto MysqlDatabase::DoCheckoutWithOperation(MYSQL* conn, const DatabaseCheckoutOperation& operation,
                                            bool acquire) -> GetResult {
  GetResult result;
  if (!operation.IsValid()) {
    result.error = "invalid_checkout_operation";
    result.error_kind = DatabaseErrorKind::kOperationConflict;
    return result;
  }
  auto hash = HashCheckoutOperation(operation);
  if (!hash) {
    result.error = hash.Error().Message();
    return result;
  }
  if (auto begun = BeginTxn(conn); !begun) {
    result.error = begun.Error().Message();
    return result;
  }
  bool acquired = false;
  auto execute = [&]() -> GetResult {
    GetResult outcome;
    auto previous = LockOperationReceipt(conn, operation.id, *hash);
    if (!previous) {
      outcome.error = previous.Error().Message();
      outcome.error_kind = previous.Error().Code() == ErrorCode::kAlreadyExists
                               ? DatabaseErrorKind::kOperationConflict
                               : DatabaseErrorKind::kCommitUnknown;
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
      if (auto active = CheckCheckoutProcess(conn, operation.owner); !active) {
        outcome.error = active.Error().Message();
        outcome.error_kind = active.Error().Code() == ErrorCode::kPermissionDenied
                                 ? DatabaseErrorKind::kOwnershipConflict
                                 : DatabaseErrorKind::kNone;
        return outcome;
      }
    }
    auto row = *previous ? FetchByDbid(conn, (**previous).dbid, operation.type_id, true)
                         : (operation.dbid == kInvalidDBID
                                ? FetchByName(conn, operation.type_id, operation.identifier, true)
                                : FetchByDbid(conn, operation.dbid, operation.type_id, true));
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
    auto version =
        AcquireCheckout(conn, outcome.data.dbid, operation.type_id, operation.owner, UnixTimeMs());
    if (!version) {
      outcome = GetResult{};
      outcome.error = version.Error().Message();
      return outcome;
    }
    auto saved = SaveOperationReceipt(conn, operation.id, {*hash, outcome.data.dbid, *version});
    if (!saved) {
      outcome = GetResult{};
      outcome.error = saved.Error().Message();
      return outcome;
    }
    acquired = true;
    outcome.checkout_version = *version;
    return outcome;
  };
  result = execute();
  if (acquired) {
    if (auto committed = Commit(conn); !committed) {
      Rollback(conn);
      result = GetResult{};
      result.error = committed.Error().Message();
      result.error_kind = DatabaseErrorKind::kCommitUnknown;
    }
  } else {
    Rollback(conn);
  }
  return result;
}

auto MysqlDatabase::DoPutWithOperation(MYSQL* conn, const DatabaseWriteOperation& operation,
                                       DatabaseID dbid, uint16_t type_id, WriteFlags flags,
                                       std::span<const std::byte> blob,
                                       const std::string& identifier) -> PutResult {
  PutResult result;
  const bool allocated_create = HasFlag(operation.requested_flags, WriteFlags::kCreateNew) &&
                                operation.requested_dbid == kInvalidDBID;
  if (!operation.IsValid() || (!allocated_create && dbid != operation.requested_dbid) ||
      (flags != operation.requested_flags &&
       !(allocated_create && flags == (operation.requested_flags | WriteFlags::kExplicitDbid)))) {
    result.error = "invalid_write_operation";
    result.error_kind = DatabaseErrorKind::kOperationConflict;
    return result;
  }
  const bool creating = HasFlag(operation.requested_flags, WriteFlags::kCreateNew);
  if (operation.create_owner &&
      (!creating || operation.expected_owner || operation.create_owner->ownership_version != 0 ||
       operation.create_owner->base_addr.Port() == 0 || operation.entity_id == 0 ||
       operation.create_owner->entity_id != operation.entity_id ||
       HasFlag(flags, WriteFlags::kLogOff))) {
    result.error = "invalid_create_owner";
    result.error_kind = DatabaseErrorKind::kOperationConflict;
    return result;
  }

  auto digest = HashWriteOperation(operation, type_id, blob, identifier);
  if (!digest) {
    result.error = digest.Error().Message();
    return result;
  }
  auto begin = BeginTxn(conn);
  if (!begin) {
    result.error = std::string(begin.Error().Message());
    return result;
  }
  auto execute = [&]() -> PutResult {
    PutResult outcome;
    auto previous = LockOperationReceipt(conn, operation.id, *digest);
    if (!previous) {
      outcome.error = previous.Error().Message();
      outcome.error_kind = previous.Error().Code() == ErrorCode::kAlreadyExists
                               ? DatabaseErrorKind::kOperationConflict
                               : DatabaseErrorKind::kCommitUnknown;
      return outcome;
    }
    if (*previous) {
      if (operation.create_owner.has_value() != ((**previous).checkout_version != 0)) {
        outcome.error = "invalid write receipt ownership";
        outcome.error_kind = DatabaseErrorKind::kCommitUnknown;
        return outcome;
      }
      return RestoreWriteReceipt(operation, **previous);
    }
    if (operation.create_owner) {
      if (auto active = CheckCheckoutProcess(conn, *operation.create_owner); !active) {
        outcome.error = active.Error().Message();
        outcome.error_kind = active.Error().Code() == ErrorCode::kPermissionDenied
                                 ? DatabaseErrorKind::kOwnershipConflict
                                 : DatabaseErrorKind::kNone;
        return outcome;
      }
    }
    if (!allocated_create) {
      auto current = FetchByDbid(conn, dbid, type_id, true);
      if (!current) {
        outcome.error = std::string(current.Error().Message());
        return outcome;
      }
      const auto& owner = current->checked_out_by;
      if (!operation.expected_owner) {
        if (owner) {
          outcome.error = "missing_checkout_version";
          outcome.error_kind = DatabaseErrorKind::kOwnershipConflict;
          return outcome;
        }
      } else {
        const auto& expected = *operation.expected_owner;
        if (!owner || expected.ownership_version == 0 ||
            owner->ownership_version != expected.ownership_version ||
            owner->base_addr != expected.base_addr || owner->app_id != expected.app_id ||
            owner->entity_id != expected.entity_id ||
            owner->process_instance != expected.process_instance) {
          outcome.error = "checkout_owner_mismatch";
          outcome.error_kind = DatabaseErrorKind::kOwnershipConflict;
          return outcome;
        }
      }
    }
    outcome = DoPut(conn, dbid, type_id, flags, blob, identifier, false, {}, true);
    if (!outcome.success) return outcome;
    if (operation.create_owner) {
      auto version =
          AcquireCheckout(conn, outcome.dbid, type_id, *operation.create_owner, UnixTimeMs());
      if (!version) {
        outcome.success = false;
        outcome.error = std::string(version.Error().Message());
        return outcome;
      }
      outcome.checkout_owner = operation.create_owner;
      outcome.checkout_owner->ownership_version = *version;
    }
    auto recorded = SaveOperationReceipt(
        conn, operation.id,
        {*digest, outcome.dbid,
         outcome.checkout_owner ? outcome.checkout_owner->ownership_version : 0});
    if (!recorded) {
      outcome.success = false;
      outcome.error = std::string(recorded.Error().Message());
    }
    return outcome;
  };
  result = execute();
  if (!result.success) {
    result.checkout_owner.reset();
    Rollback(conn);
    return result;
  }
  auto committed = Commit(conn);
  if (!committed) {
    Rollback(conn);
    result.success = false;
    result.checkout_owner.reset();
    result.error = std::string(committed.Error().Message());
    result.error_kind = DatabaseErrorKind::kCommitUnknown;
  }
  return result;
}

auto MysqlDatabase::DoGet(MYSQL* conn, DatabaseID dbid, uint16_t type_id) -> GetResult {
  GetResult result;
  auto row_result = FetchByDbid(conn, dbid, type_id);
  if (!row_result) {
    result.error = std::string(row_result.Error().Message());
    return result;
  }
  auto row = std::move(*row_result);
  if (!row.found) {
    result.error_kind = DatabaseErrorKind::kNotFound;
    result.error = std::format("entity ({},{}) not found", type_id, dbid);
    return result;
  }
  result.success = true;
  result.data = std::move(row.data);
  result.checked_out_by = row.checked_out_by;
  return result;
}

auto MysqlDatabase::DoDel(MYSQL* conn, DatabaseID dbid, uint16_t type_id, bool transaction_open)
    -> DelResult {
  if (!transaction_open) {
    if (auto begun = BeginTxn(conn); !begun)
      return DelResult{false, std::string(begun.Error().Message())};
  }
  auto remove = [&]() -> Result<void> {
    auto row = FetchByDbid(conn, dbid, type_id, true);
    if (!row) return row.Error();
    if (!row->found) return Error{ErrorCode::kNotFound, "delete entity not found"};
    auto session = DoReadAccountSession(conn, dbid, type_id, true);
    if (!session) return session.Error();
    if (session->phase != AccountSessionPhase::kOffline)
      return Error{ErrorCode::kPermissionDenied,
                   "account session must be released before deletion"};
    return ExecSql(conn, std::format("DELETE FROM entities WHERE dbid = {} AND type_id = {}",
                                     static_cast<uint64_t>(dbid), type_id));
  };
  auto result = remove();
  if (!transaction_open) {
    if (result) result = Commit(conn);
    if (!result) Rollback(conn);
  }
  if (result) return DelResult{true, {}};
  const auto code = result.Error().Code();
  const auto kind = code == ErrorCode::kPermissionDenied ? DatabaseErrorKind::kOwnershipConflict
                    : code == ErrorCode::kCommitUnknown  ? DatabaseErrorKind::kCommitUnknown
                    : code == ErrorCode::kNotFound       ? DatabaseErrorKind::kNotFound
                                                         : DatabaseErrorKind::kNone;
  return DelResult{false, std::string(result.Error().Message()), kind};
}

auto MysqlDatabase::DoLookup(MYSQL* conn, uint16_t type_id, const std::string& identifier)
    -> LookupResult {
  LookupResult result;
  auto row_result = FetchByName(conn, type_id, identifier);
  if (!row_result) {
    result.error = std::string(row_result.Error().Message());
    return result;
  }
  auto row = std::move(*row_result);
  if (row.found) {
    result.found = true;
    result.dbid = row.data.dbid;
    result.password_hash = std::move(row.password_hash);
  }
  return result;
}

auto MysqlDatabase::AcquireCheckout(MYSQL* conn, DatabaseID dbid, uint16_t type_id,
                                    const CheckoutInfo& new_owner, int64_t now_ms)
    -> Result<uint64_t> {
  auto next = ExecSql(
      conn,
      std::format("INSERT INTO atlas_checkout_versions(dbid, type_id, version) VALUES({}, {}, 1) "
                  "ON DUPLICATE KEY UPDATE version = version + 1",
                  static_cast<uint64_t>(dbid), type_id));
  if (!next) {
    return next.Error();
  }
  auto upd = ExecSql(
      conn,
      std::format(
          "UPDATE entities SET checked_out = 1, checkout_ip = {}, checkout_port = {}, "
          "checkout_app_id = {}, checkout_eid = {}, checkout_instance = {}, updated_at_ms = {}, "
          "checkout_version = (SELECT version FROM atlas_checkout_versions "
          "WHERE dbid = {} AND type_id = {}) "
          "WHERE dbid = {} AND type_id = {} AND checked_out = 0",
          new_owner.base_addr.Ip(), new_owner.base_addr.Port(), new_owner.app_id,
          new_owner.entity_id, new_owner.process_instance, now_ms, static_cast<uint64_t>(dbid),
          type_id, static_cast<uint64_t>(dbid), type_id));
  if (!upd) {
    return upd.Error();
  }
  if (mysql_affected_rows(conn) != 1) {
    return Error{ErrorCode::kPermissionDenied, "checkout conflict"};
  }
  auto acquired = FetchByDbid(conn, dbid, type_id);
  if (!acquired || !acquired->checked_out_by) {
    return Error{ErrorCode::kIoError, "checkout owner readback failed"};
  }
  return acquired->checkout_version;
}

auto MysqlDatabase::CheckoutTail(MYSQL* conn, DatabaseID dbid, uint16_t type_id, EntityRow row,
                                 const CheckoutInfo& new_owner, int64_t now_ms) -> GetResult {
  GetResult result;
  auto acquired = AcquireCheckout(conn, dbid, type_id, new_owner, now_ms);
  if (!acquired) {
    Rollback(conn);
    result.error = std::string(acquired.Error().Message());
    return result;
  }
  if (auto committed = Commit(conn); !committed) {
    Rollback(conn);
    result.error = std::string(committed.Error().Message());
    result.error_kind = DatabaseErrorKind::kCommitUnknown;
    return result;
  }
  result.success = true;
  result.data = std::move(row.data);
  result.checkout_version = *acquired;
  return result;
}

auto MysqlDatabase::DoCheckout(MYSQL* conn, DatabaseID dbid, uint16_t type_id,
                               const CheckoutInfo& new_owner) -> GetResult {
  GetResult result;
  const auto now = UnixTimeMs();
  if (auto begin = BeginTxn(conn); !begin) {
    result.error = std::string(begin.Error().Message());
    return result;
  }
  if (auto active = CheckCheckoutProcess(conn, new_owner); !active) {
    Rollback(conn);
    result.error = active.Error().Message();
    result.error_kind = active.Error().Code() == ErrorCode::kPermissionDenied
                            ? DatabaseErrorKind::kOwnershipConflict
                            : DatabaseErrorKind::kNone;
    return result;
  }
  auto row_result = FetchByDbid(conn, dbid, type_id, true);
  if (!row_result) {
    Rollback(conn);
    result.error = std::string(row_result.Error().Message());
    return result;
  }
  auto row = std::move(*row_result);
  if (!row.found) {
    Rollback(conn);
    result.error_kind = DatabaseErrorKind::kNotFound;
    result.error = std::format("checkout: entity ({},{}) not found", type_id, dbid);
    return result;
  }
  if (row.checked_out_by.has_value()) {
    Rollback(conn);
    result.success = true;
    result.data = std::move(row.data);
    result.checked_out_by = row.checked_out_by;
    return result;
  }
  return CheckoutTail(conn, dbid, type_id, std::move(row), new_owner, now);
}

auto MysqlDatabase::DoCheckoutByName(MYSQL* conn, uint16_t type_id, const std::string& identifier,
                                     const CheckoutInfo& new_owner) -> GetResult {
  GetResult result;
  const auto now = UnixTimeMs();
  if (auto begin = BeginTxn(conn); !begin) {
    result.error = std::string(begin.Error().Message());
    return result;
  }
  if (auto active = CheckCheckoutProcess(conn, new_owner); !active) {
    Rollback(conn);
    result.error = active.Error().Message();
    result.error_kind = active.Error().Code() == ErrorCode::kPermissionDenied
                            ? DatabaseErrorKind::kOwnershipConflict
                            : DatabaseErrorKind::kNone;
    return result;
  }
  auto lookup = FetchByName(conn, type_id, identifier, true);
  if (!lookup) {
    Rollback(conn);
    result.error = std::string(lookup.Error().Message());
    return result;
  }
  auto row = std::move(*lookup);
  if (!row.found) {
    Rollback(conn);
    result.error_kind = DatabaseErrorKind::kNotFound;
    result.error = std::format("checkout_by_name: '{}' not found", identifier);
    return result;
  }
  if (row.checked_out_by.has_value()) {
    Rollback(conn);
    result.success = true;
    result.data = std::move(row.data);
    result.checked_out_by = row.checked_out_by;
    return result;
  }
  const auto resolved_dbid = row.data.dbid;
  return CheckoutTail(conn, resolved_dbid, type_id, std::move(row), new_owner, now);
}

auto MysqlDatabase::DoClearCheckout(MYSQL* conn, DatabaseID dbid, uint16_t type_id)
    -> Result<bool> {
  auto upd = ExecSql(
      conn, std::format("UPDATE entities SET checked_out = 0, checkout_ip = 0, checkout_port = 0, "
                        "checkout_app_id = 0, checkout_eid = 0, updated_at_ms = {} "
                        "WHERE dbid = {} AND type_id = {} AND checked_out = 1",
                        UnixTimeMs(), static_cast<uint64_t>(dbid), type_id));
  if (!upd) return upd.Error();
  return mysql_affected_rows(conn) > 0;
}

auto MysqlDatabase::DoClearForAddress(MYSQL* conn, const Address& base_addr) -> Result<int> {
  auto upd = ExecSql(
      conn, std::format("UPDATE entities SET checked_out = 0, checkout_ip = 0, checkout_port = 0, "
                        "checkout_app_id = 0, checkout_eid = 0, updated_at_ms = {} "
                        "WHERE checked_out = 1 AND checkout_ip = {} AND checkout_port = {}",
                        UnixTimeMs(), base_addr.Ip(), base_addr.Port()));
  if (!upd) return upd.Error();
  return static_cast<int>(mysql_affected_rows(conn));
}

auto MysqlDatabase::DoGetAutoLoad(MYSQL* conn) -> Result<std::vector<EntityData>> {
  std::vector<EntityData> result;
  static constexpr std::string_view kSql =
      "SELECT dbid, type_id, `blob`, identifier FROM entities WHERE auto_load = 1 "
      "ORDER BY type_id, dbid";
  if (conn == nullptr ||
      mysql_real_query(conn, kSql.data(), static_cast<unsigned long>(kSql.size())) != 0) {
    return MysqlErr(conn, "auto-load query failed");
  }
  MYSQL_RES* res = mysql_store_result(conn);
  if (res == nullptr) return MysqlErr(conn, "auto-load results unavailable");
  while (MYSQL_ROW row = mysql_fetch_row(res)) {
    const unsigned long* len = mysql_fetch_lengths(res);
    EntityData data;
    data.dbid = static_cast<DatabaseID>(std::strtoull(row[0], nullptr, 10));
    data.type_id = static_cast<uint16_t>(std::strtoul(row[1], nullptr, 10));
    if (row[2] != nullptr && len[2] > 0) {
      const auto* bytes = reinterpret_cast<const std::byte*>(row[2]);
      data.blob.assign(bytes, bytes + len[2]);
    }
    if (row[3] != nullptr) data.identifier.assign(row[3], len[3]);
    result.push_back(std::move(data));
  }
  mysql_free_result(res);
  return result;
}

auto MysqlDatabase::DoSetAutoLoad(MYSQL* conn, DatabaseID dbid, uint16_t type_id, bool auto_load)
    -> Result<void> {
  return ExecSql(
      conn, std::format("UPDATE entities SET auto_load = {}, updated_at_ms = {} "
                        "WHERE dbid = {} AND type_id = {}",
                        auto_load ? 1 : 0, UnixTimeMs(), static_cast<uint64_t>(dbid), type_id));
}

auto MysqlDatabase::DoGetMaxDbid(MYSQL* conn, DatabaseID low, DatabaseID high) -> DbidRangeResult {
  DbidRangeResult result;
  if (high <= low) {
    result.success = true;
    return result;
  }
  auto sql = std::format(
      "SELECT MAX(dbid) FROM ("
      "SELECT MAX(dbid) AS dbid FROM entities WHERE dbid >= {0} AND dbid < {1} UNION ALL "
      "SELECT MAX(dbid) FROM atlas_checkout_versions WHERE dbid >= {0} AND dbid < {1} UNION ALL "
      "SELECT MAX(result_dbid) FROM atlas_write_receipts "
      "WHERE result_dbid >= {0} AND result_dbid < {1}) AS allocated",
      static_cast<uint64_t>(low), static_cast<uint64_t>(high));
  if (conn == nullptr ||
      mysql_real_query(conn, sql.data(), static_cast<unsigned long>(sql.size())) != 0) {
    result.error = std::string(MysqlErr(conn, "MysqlDatabase: max dbid query failed").Message());
    return result;
  }
  MYSQL_RES* res = mysql_store_result(conn);
  if (res == nullptr) {
    result.error =
        std::string(MysqlErr(conn, "MysqlDatabase: max dbid store_result failed").Message());
    return result;
  }
  result.success = true;
  if (MYSQL_ROW row = mysql_fetch_row(res); row != nullptr && row[0] != nullptr) {
    result.max_dbid = static_cast<DatabaseID>(std::strtoull(row[0], nullptr, 10));
  }
  mysql_free_result(res);
  return result;
}

auto MysqlDatabase::DoReserveEntityIds(MYSQL* conn, uint32_t count) -> Result<EntityIdRange> {
  if (count == 0 || count >= kFirstLocalEntityID) {
    return Error{ErrorCode::kOutOfRange, "invalid EntityID reservation count"};
  }
  auto begin = BeginTxn(conn);
  if (!begin) return begin.Error();
  auto reserve = [&]() -> Result<EntityIdRange> {
    constexpr std::string_view query =
        "SELECT next_id FROM atlas_entity_id_counter WHERE id = 1 FOR UPDATE";
    if (mysql_real_query(conn, query.data(), static_cast<unsigned long>(query.size())) != 0) {
      return MysqlErr(conn, "EntityID reservation read failed");
    }
    std::unique_ptr<MYSQL_RES, decltype(&mysql_free_result)> rows(mysql_store_result(conn),
                                                                  mysql_free_result);
    if (!rows) return MysqlErr(conn, "EntityID reservation result failed");
    MYSQL_ROW row = mysql_fetch_row(rows.get());
    if (row == nullptr || row[0] == nullptr) {
      return Error{ErrorCode::kIoError, "EntityID counter row is missing"};
    }
    const std::string_view text(row[0]);
    uint64_t next = 0;
    auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), next);
    if (error != std::errc{} || end != text.data() + text.size() || next == 0 ||
        next > kFirstLocalEntityID || count > kFirstLocalEntityID - next) {
      return Error{ErrorCode::kOutOfRange, "EntityID counter is invalid or exhausted"};
    }
    rows.reset();
    auto update = ExecSql(
        conn,
        std::format("UPDATE atlas_entity_id_counter SET next_id = {} WHERE id = 1", next + count));
    if (!update) return update.Error();
    return EntityIdRange{static_cast<EntityID>(next), static_cast<EntityID>(next + count - 1)};
  };
  auto result = reserve();
  if (!result) {
    Rollback(conn);
    return result;
  }
  auto committed = Commit(conn);
  if (!committed) {
    Rollback(conn);
    return committed.Error();
  }
  return result;
}

// ── public API: dispatch onto the pool (or run inline when synchronous) ─────

void MysqlDatabase::PutEntity(DatabaseID dbid, uint16_t type_id, WriteFlags flags,
                              std::span<const std::byte> blob, const std::string& identifier,
                              std::function<void(PutResult)> callback) {
  std::vector<std::byte> owned(blob.begin(), blob.end());
  Dispatch<PutResult>(
      type_id, dbid,
      [dbid, type_id, flags, b = std::move(owned), id = identifier](MYSQL* conn) {
        return DoPut(conn, dbid, type_id, flags, b, id, false, {});
      },
      std::move(callback));
}

void MysqlDatabase::PutEntityWithOperation(const DatabaseWriteOperation& operation, DatabaseID dbid,
                                           uint16_t type_id, WriteFlags flags,
                                           std::span<const std::byte> blob,
                                           const std::string& identifier,
                                           std::function<void(PutResult)> callback) {
  std::vector<std::byte> owned(blob.begin(), blob.end());
  Dispatch<PutResult>(
      type_id, dbid,
      [operation, dbid, type_id, flags, data = std::move(owned), identifier](MYSQL* conn) {
        return DoPutWithOperation(conn, operation, dbid, type_id, flags, data, identifier);
      },
      std::move(callback));
}

void MysqlDatabase::PutEntityWithPassword(DatabaseID dbid, uint16_t type_id, WriteFlags flags,
                                          std::span<const std::byte> blob,
                                          const std::string& identifier,
                                          const std::string& password_hash,
                                          std::function<void(PutResult)> callback) {
  std::vector<std::byte> owned(blob.begin(), blob.end());
  Dispatch<PutResult>(
      type_id, dbid,
      [dbid, type_id, flags, b = std::move(owned), id = identifier, pw = password_hash](
          MYSQL* conn) { return DoPut(conn, dbid, type_id, flags, b, id, true, pw); },
      std::move(callback));
}

void MysqlDatabase::GetEntity(DatabaseID dbid, uint16_t type_id,
                              std::function<void(GetResult)> callback) {
  Dispatch<GetResult>(
      type_id, dbid, [dbid, type_id](MYSQL* conn) { return DoGet(conn, dbid, type_id); },
      std::move(callback));
}

void MysqlDatabase::DelEntity(DatabaseID dbid, uint16_t type_id,
                              std::function<void(DelResult)> callback) {
  Dispatch<DelResult>(
      type_id, dbid, [dbid, type_id](MYSQL* conn) { return DoDel(conn, dbid, type_id); },
      std::move(callback));
}

void MysqlDatabase::LookupByName(uint16_t type_id, const std::string& identifier,
                                 std::function<void(LookupResult)> callback) {
  Dispatch<LookupResult>(
      type_id, std::hash<std::string>{}(identifier),
      [type_id, id = identifier](MYSQL* conn) { return DoLookup(conn, type_id, id); },
      std::move(callback));
}

void MysqlDatabase::CheckoutEntity(DatabaseID dbid, uint16_t type_id, const CheckoutInfo& new_owner,
                                   std::function<void(GetResult)> callback) {
  Dispatch<GetResult>(
      type_id, dbid,
      [dbid, type_id, new_owner](MYSQL* conn) {
        return DoCheckout(conn, dbid, type_id, new_owner);
      },
      std::move(callback));
}

void MysqlDatabase::CheckoutWithOperation(const DatabaseCheckoutOperation& operation,
                                          std::function<void(GetResult)> callback) {
  Dispatch<GetResult>(
      operation.type_id, operation.dbid,
      [operation](MYSQL* conn) { return DoCheckoutWithOperation(conn, operation, true); },
      std::move(callback));
}

void MysqlDatabase::ResolveCheckoutOperation(const DatabaseCheckoutOperation& operation,
                                             std::function<void(GetResult)> callback) {
  Dispatch<GetResult>(
      operation.type_id, operation.dbid,
      [operation](MYSQL* conn) { return DoCheckoutWithOperation(conn, operation, false); },
      std::move(callback));
}

void MysqlDatabase::CheckoutEntityByName(uint16_t type_id, const std::string& identifier,
                                         const CheckoutInfo& new_owner,
                                         std::function<void(GetResult)> callback) {
  Dispatch<GetResult>(
      type_id, std::hash<std::string>{}(identifier),
      [type_id, id = identifier, new_owner](MYSQL* conn) {
        return DoCheckoutByName(conn, type_id, id, new_owner);
      },
      std::move(callback));
}

void MysqlDatabase::ReleaseCheckout(DatabaseID dbid, uint16_t type_id, const CheckoutInfo& owner,
                                    std::function<void(Result<void>)> callback) {
  Dispatch<Result<void>>(
      type_id, dbid,
      [dbid, type_id, owner](MYSQL* conn) -> Result<void> {
        if (owner.ownership_version == 0) {
          return Error{ErrorCode::kPermissionDenied, "missing checkout version"};
        }
        auto update = ExecSql(
            conn,
            std::format("UPDATE entities SET checked_out = 0, checkout_ip = 0, checkout_port = 0, "
                        "checkout_app_id = 0, checkout_eid = 0 WHERE dbid = {} AND type_id = {} "
                        "AND checked_out = 1 AND checkout_ip = {} AND checkout_port = {} "
                        "AND checkout_app_id = {} AND checkout_eid = {} AND checkout_version = {} "
                        "AND checkout_instance = {}",
                        static_cast<uint64_t>(dbid), type_id, owner.base_addr.Ip(),
                        owner.base_addr.Port(), owner.app_id, owner.entity_id,
                        owner.ownership_version, owner.process_instance));
        if (!update) return update.Error();
        if (mysql_affected_rows(conn) != 1) {
          return Error{ErrorCode::kPermissionDenied, "checkout owner mismatch"};
        }
        return {};
      },
      std::move(callback));
}

auto MysqlDatabase::CheckCheckoutProcess(MYSQL* conn, const CheckoutInfo& owner) -> Result<void> {
  if (owner.process_instance == 0) return {};
  if (owner.process_instance > static_cast<uint64_t>((std::numeric_limits<int64_t>::max)()))
    return Error{ErrorCode::kInvalidArgument, "invalid checkout process instance"};
  auto inserted = ExecSql(
      conn,
      std::format(
          "INSERT INTO atlas_checkout_processes(base_ip, base_port, instance) VALUES({}, {}, {}) "
          "ON DUPLICATE KEY UPDATE retired = retired",
          owner.base_addr.Ip(), owner.base_addr.Port(), owner.process_instance));
  if (!inserted) return inserted.Error();
  auto sql = std::format(
      "SELECT retired FROM atlas_checkout_processes "
      "WHERE base_ip = {} AND base_port = {} AND instance = {} FOR UPDATE",
      owner.base_addr.Ip(), owner.base_addr.Port(), owner.process_instance);
  if (mysql_real_query(conn, sql.data(), static_cast<unsigned long>(sql.size())) != 0)
    return MysqlErr(conn, "checkout process lookup failed");
  auto* rows = mysql_store_result(conn);
  if (!rows) return MysqlErr(conn, "checkout process result missing");
  auto row = mysql_fetch_row(rows);
  const bool active = row && row[0] && std::string_view(row[0]) == "0";
  mysql_free_result(rows);
  if (!active) return Error{ErrorCode::kPermissionDenied, "checkout process retired"};
  return {};
}

void MysqlDatabase::RetireCheckoutProcess(const Address& base_addr, uint64_t instance,
                                          std::function<void(Result<int>)> callback) {
  Dispatch<Result<int>>(
      0, 0,
      [base_addr, instance](MYSQL* conn) -> Result<int> {
        if (instance == 0 ||
            instance > static_cast<uint64_t>((std::numeric_limits<int64_t>::max)()))
          return Error{ErrorCode::kInvalidArgument, "invalid checkout process instance"};
        if (auto begun = BeginTxn(conn); !begun) return begun.Error();
        auto retire = [&]() -> Result<int> {
          auto marked = ExecSql(
              conn,
              std::format(
                  "INSERT INTO atlas_checkout_processes(base_ip, base_port, instance, retired) "
                  "VALUES({}, {}, {}, 1) ON DUPLICATE KEY UPDATE retired = 1",
                  base_addr.Ip(), base_addr.Port(), instance));
          if (!marked) return marked.Error();
          auto cleared = ExecSql(
              conn, std::format(
                        "UPDATE entities SET checked_out = 0, checkout_ip = 0, checkout_port = 0, "
                        "checkout_app_id = 0, checkout_eid = 0, checkout_instance = 0 "
                        "WHERE checked_out = 1 AND checkout_ip = {} AND checkout_port = {} "
                        "AND checkout_instance = {}",
                        base_addr.Ip(), base_addr.Port(), instance));
          if (!cleared) return cleared.Error();
          return static_cast<int>(mysql_affected_rows(conn));
        };
        auto result = retire();
        if (!result) {
          Rollback(conn);
          return result.Error();
        }
        auto committed = Commit(conn);
        if (!committed) {
          Rollback(conn);
          return committed.Error();
        }
        return result;
      },
      std::move(callback));
}

void MysqlDatabase::ClearCheckout(DatabaseID dbid, uint16_t type_id,
                                  std::function<void(Result<bool>)> callback) {
  Dispatch<Result<bool>>(
      type_id, dbid, [dbid, type_id](MYSQL* conn) { return DoClearCheckout(conn, dbid, type_id); },
      std::move(callback));
}

void MysqlDatabase::ClearCheckoutsForAddress(const Address& base_addr,
                                             std::function<void(Result<int>)> callback) {
  Dispatch<Result<int>>(
      0, 0, [base_addr](MYSQL* conn) { return DoClearForAddress(conn, base_addr); },
      std::move(callback));
}

void MysqlDatabase::GetAutoLoadEntities(
    std::function<void(Result<std::vector<EntityData>>)> callback) {
  Dispatch<Result<std::vector<EntityData>>>(
      0, 0, [](MYSQL* conn) { return DoGetAutoLoad(conn); }, std::move(callback));
}

void MysqlDatabase::SetAutoLoad(DatabaseID dbid, uint16_t type_id, bool auto_load,
                                std::function<void(Result<void>)> callback) {
  Dispatch<Result<void>>(
      type_id, dbid,
      [dbid, type_id, auto_load](MYSQL* conn) {
        return DoSetAutoLoad(conn, dbid, type_id, auto_load);
      },
      std::move(callback));
}

void MysqlDatabase::GetMaxDbidInRange(DatabaseID low, DatabaseID high,
                                      std::function<void(DbidRangeResult)> callback) {
  Dispatch<DbidRangeResult>(
      0, 0, [low, high](MYSQL* conn) { return DoGetMaxDbid(conn, low, high); },
      std::move(callback));
}

void MysqlDatabase::ReserveEntityIds(uint32_t count,
                                     std::function<void(Result<EntityIdRange>)> callback) {
  Dispatch<Result<EntityIdRange>>(
      0, 0, [count](MYSQL* conn) { return DoReserveEntityIds(conn, count); }, std::move(callback));
}

}  // namespace atlas
