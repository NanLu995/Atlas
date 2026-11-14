#ifndef ATLAS_LIB_DB_SQLITE_SQLITE_DATABASE_H_
#define ATLAS_LIB_DB_SQLITE_SQLITE_DATABASE_H_

#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <sqlite3.h>

#include "db/write_operation.h"
#include "platform/filesystem.h"

namespace atlas {

class SqliteDatabase : public IDatabase {
 public:
  SqliteDatabase() = default;
  ~SqliteDatabase() override;

  void SetDeferredMode(bool enabled) override { deferred_mode_ = enabled; }

  [[nodiscard]] auto Startup(const DatabaseConfig& config, const EntityDefRegistry& entity_defs)
      -> Result<void> override;
  void Shutdown() override;

  void PutEntity(DatabaseID dbid, uint16_t type_id, WriteFlags flags,
                 std::span<const std::byte> blob, const std::string& identifier,
                 std::function<void(PutResult)> callback) override;

  void PutEntityWithOperation(const DatabaseWriteOperation& operation, DatabaseID dbid,
                              uint16_t type_id, WriteFlags flags, std::span<const std::byte> blob,
                              const std::string& identifier,
                              std::function<void(PutResult)> callback) override;

  void PutEntityWithPassword(DatabaseID dbid, uint16_t type_id, WriteFlags flags,
                             std::span<const std::byte> blob, const std::string& identifier,
                             const std::string& password_hash,
                             std::function<void(PutResult)> callback) override;

  void GetEntity(DatabaseID dbid, uint16_t type_id,
                 std::function<void(GetResult)> callback) override;

  void DelEntity(DatabaseID dbid, uint16_t type_id,
                 std::function<void(DelResult)> callback) override;

  void LookupByName(uint16_t type_id, const std::string& identifier,
                    std::function<void(LookupResult)> callback) override;

  void CheckoutEntity(DatabaseID dbid, uint16_t type_id, const CheckoutInfo& new_owner,
                      std::function<void(GetResult)> callback) override;

  void CheckoutEntityByName(uint16_t type_id, const std::string& identifier,
                            const CheckoutInfo& new_owner,
                            std::function<void(GetResult)> callback) override;
  void CheckoutWithOperation(const DatabaseCheckoutOperation& operation,
                             std::function<void(GetResult)> callback) override;
  void ResolveCheckoutOperation(const DatabaseCheckoutOperation& operation,
                                std::function<void(GetResult)> callback) override;

  void ReleaseCheckout(DatabaseID dbid, uint16_t type_id, const CheckoutInfo& owner,
                       std::function<void(Result<void>)> callback) override;

  void ClearCheckout(DatabaseID dbid, uint16_t type_id,
                     std::function<void(bool)> callback) override;

  void ClearCheckoutsForAddress(const Address& base_addr,
                                std::function<void(int cleared_count)> callback) override;

  void RetireCheckoutProcess(const Address& base_addr, uint64_t instance,
                             std::function<void(Result<int>)> callback) override;

  void MarkCheckoutCleared(DatabaseID dbid, uint16_t type_id) override;

  void GetAutoLoadEntities(std::function<void(std::vector<EntityData>)> callback) override;

  void SetAutoLoad(DatabaseID dbid, uint16_t type_id, bool auto_load) override;

  void GetMaxDbidInRange(DatabaseID low, DatabaseID high,
                         std::function<void(DbidRangeResult)> callback) override;

  void ReserveEntityIds(uint32_t count,
                        std::function<void(Result<EntityIdRange>)> callback) override;

  void ProcessResults() override;

  void ReadAccountSession(DatabaseID dbid, uint16_t type_id,
                          std::function<void(Result<AccountSession>)> callback) override;
  void UpdateAccountSession(const AccountSessionOperation& operation,
                            std::function<void(Result<AccountSession>)> callback) override;

  void BeginBatch() override;
  void EndBatch() override;

 private:
  friend struct SqliteDatabaseTestAccess;
  struct BatchStatus {
    std::optional<Error> error;
  };
  struct EntityRow {
    bool found{false};
    EntityData data;
    std::string password_hash;
    bool auto_load{false};
    std::optional<CheckoutInfo> checked_out_by;
  };

  class Statement {
   public:
    Statement() = default;
    explicit Statement(sqlite3_stmt* stmt) : stmt_(stmt) {}
    Statement(const Statement&) = delete;
    auto operator=(const Statement&) -> Statement& = delete;
    Statement(Statement&& other) noexcept;
    auto operator=(Statement&& other) noexcept -> Statement&;
    ~Statement();

    [[nodiscard]] auto Get() const -> sqlite3_stmt* { return stmt_; }
    void Reset();

   private:
    sqlite3_stmt* stmt_{nullptr};
  };

  [[nodiscard]] auto OpenDatabase(const DatabaseConfig& config) -> Result<void>;
  [[nodiscard]] auto EnsureSchema() -> Result<void>;
  [[nodiscard]] auto ExecSql(std::string_view sql) -> Result<void>;
  [[nodiscard]] auto ExecSqlIgnoringDuplicateColumn(std::string_view sql) -> Result<void>;
  [[nodiscard]] auto Prepare(std::string_view sql) -> Result<Statement>;
  [[nodiscard]] auto UpsertMeta(std::string_view key, std::string_view value) -> Result<void>;
  [[nodiscard]] auto FetchByDbid(DatabaseID dbid, uint16_t type_id) -> Result<EntityRow>;
  [[nodiscard]] auto ReadAccountSessionImpl(DatabaseID dbid, uint16_t type_id)
      -> Result<AccountSession>;
  [[nodiscard]] auto FetchByName(uint16_t type_id, std::string_view identifier)
      -> Result<EntityRow>;
  [[nodiscard]] auto ReadRow(sqlite3_stmt* stmt) const -> Result<EntityRow>;
  [[nodiscard]] auto AcquireCheckoutVersion(DatabaseID dbid, uint16_t type_id) -> Result<uint64_t>;
  [[nodiscard]] auto CheckCheckoutProcess(const CheckoutInfo& owner) -> Result<void>;
  [[nodiscard]] auto AcquireCheckout(DatabaseID dbid, uint16_t type_id, const CheckoutInfo& owner)
      -> Result<uint64_t>;
  void CheckoutTail(EntityRow row, const CheckoutInfo& owner, const std::string& scope,
                    std::function<void(GetResult)> callback);
  [[nodiscard]] auto BindIdentifier(Statement& stmt, int index, const std::string& identifier) const
      -> Result<void>;
  [[nodiscard]] auto BindBlob(Statement& stmt, int index, std::span<const std::byte> blob) const
      -> Result<void>;
  [[nodiscard]] auto SqliteError(std::string_view prefix) const -> Error;
  [[nodiscard]] auto SqliteError(std::string_view prefix, int code) const -> Error;
  template <typename T>
  void FireOrDefer(std::function<void(T)> callback, T result) {
    auto status = batch_txn_open_ ? batch_status_ : nullptr;
    auto complete = [cb = std::move(callback), result = std::move(result), status]() mutable {
      if (status && status->error) {
        if constexpr (requires {
                        result.HasValue();
                        result.Error();
                      }) {
          result = *status->error;
        } else {
          result = T{};
          if constexpr (std::is_same_v<T, int>) result = -1;
          if constexpr (requires { result.error; }) result.error = status->error->Message();
          if constexpr (requires { result.error_kind; }) {
            result.error_kind = DatabaseErrorKind::kCommitUnknown;
          }
        }
      }
      cb(std::move(result));
    };
    if (deferred_mode_ || status)
      deferred_.push_back(std::move(complete));
    else
      complete();
  }

  void PutEntityImpl(const DatabaseWriteOperation* operation, DatabaseID dbid, uint16_t type_id,
                     WriteFlags flags, std::span<const std::byte> blob,
                     const std::string& identifier, std::function<void(PutResult)> callback);
  void CheckoutOperationImpl(const DatabaseCheckoutOperation& operation, bool acquire,
                             std::function<void(GetResult)> callback);
  [[nodiscard]] auto FindOperationReceipt(const std::array<std::byte, 16>& id)
      -> Result<std::optional<DatabaseOperationReceipt>>;
  [[nodiscard]] auto SaveOperationReceipt(const std::array<std::byte, 16>& id,
                                          const DatabaseOperationReceipt& receipt) -> Result<void>;
  void FinishWrite(const std::string& scope, const DatabaseWriteOperation* operation,
                   const std::optional<Sha256Digest>& hash, PutResult result,
                   std::function<void(PutResult)> callback);
  [[nodiscard]] auto BeginWriteScope() -> Result<std::string>;
  [[nodiscard]] auto CommitWriteScope(const std::string& scope) -> Result<void>;
  void RollbackWriteScope(const std::string& scope);

  sqlite3* db_{nullptr};
  fs::ScopedFileLock database_lock_;
  const EntityDefRegistry* entity_defs_{nullptr};
  std::filesystem::path db_path_;
  bool started_{false};
  bool deferred_mode_{false};
  std::deque<std::function<void()>> deferred_;

  bool batch_active_{false};
  bool batch_txn_open_{false};
  std::shared_ptr<BatchStatus> batch_status_;
  int batch_savepoint_seq_{0};

  static constexpr int kMaxCallbacksPerTick = 2048;
};

}  // namespace atlas

#endif  // ATLAS_LIB_DB_SQLITE_SQLITE_DATABASE_H_
