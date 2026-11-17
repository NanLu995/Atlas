#ifndef ATLAS_LIB_DB_MYSQL_MYSQL_DATABASE_H_
#define ATLAS_LIB_DB_MYSQL_MYSQL_DATABASE_H_

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "db/database_failure.h"
#include "db/write_operation.h"
#include "foundation/callback_utils.h"

struct MYSQL;

namespace atlas {

// Deferred operations use per-entity worker connections; ProcessResults delivers main-loop
// callbacks. Synchronous callers use the bootstrap connection.
class MysqlDatabase : public IDatabase {
 public:
  MysqlDatabase() = default;
  ~MysqlDatabase() override;

  void SetDeferredMode(bool enabled) override { deferred_mode_ = enabled; }

  [[nodiscard]] auto Startup(const DatabaseConfig& config, const EntityDefRegistry& entity_defs)
      -> Result<void> override;
  void Shutdown() override;
  void BeginDrain() override;
  [[nodiscard]] auto PendingOperations() const -> std::size_t override {
    return pending_operations_.load(std::memory_order_acquire);
  }

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
                     std::function<void(Result<bool>)> callback) override;

  void ClearCheckoutsForAddress(const Address& base_addr,
                                std::function<void(Result<int>)> callback) override;

  void RetireCheckoutProcess(const Address& base_addr, uint64_t instance,
                             std::function<void(Result<int>)> callback) override;

  void GetAutoLoadEntities(std::function<void(Result<std::vector<EntityData>>)> callback) override;

  void SetAutoLoad(DatabaseID dbid, uint16_t type_id, bool auto_load,
                   std::function<void(Result<void>)> callback) override;

  void GetMaxDbidInRange(DatabaseID low, DatabaseID high,
                         std::function<void(DbidRangeResult)> callback) override;

  void ReserveEntityIds(uint32_t count,
                        std::function<void(Result<EntityIdRange>)> callback) override;

  void ReadAccountSession(DatabaseID dbid, uint16_t type_id,
                          std::function<void(Result<AccountSession>)> callback) override;
  void UpdateAccountSession(const AccountSessionOperation& operation,
                            std::function<void(Result<AccountSession>)> callback) override;

  void ProcessResults() override;

  [[nodiscard]] auto SupportsMultiDbapp() const -> bool override { return true; }

 private:
  friend struct MysqlDatabaseTestAccess;
  struct EntityRow {
    bool found{false};
    EntityData data;
    std::string password_hash;
    bool auto_load{false};
    uint64_t checkout_version{0};
    std::optional<CheckoutInfo> checked_out_by;
  };

  struct Worker {
    std::thread thread;
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<std::function<void(MYSQL*)>> jobs;
    bool stop{false};
  };

  // Deferred callbacks return through ProcessResults on the main loop.
  template <typename R>
  void Dispatch(uint16_t type_id, uint64_t key, std::function<R(MYSQL*)> op,
                std::function<void(R)> callback) {
    if (auto admitted = AdmitOperation(); !admitted) {
      callback(DatabaseFailure<R>(admitted.Error()));
      return;
    }
    if (!deferred_mode_) {
      RefreshConnection(conn_);
      callback(op(conn_));
      return;
    }
    pending_operations_.fetch_add(1, std::memory_order_relaxed);
    EnqueueForEntity(
        type_id, key, [this, op = std::move(op), cb = std::move(callback)](MYSQL* conn) mutable {
          R r = op(conn);
          PostResult([this, cb = std::move(cb), r = std::move(r)]() mutable {
            SafeInvoke("Database callback", [&] { cb(std::move(r)); });
            pending_operations_.fetch_sub(1, std::memory_order_release);
          });
        });
  }

  [[nodiscard]] auto AdmitOperation() const -> Result<void>;
  void EnqueueForEntity(uint16_t type_id, uint64_t key, std::function<void(MYSQL*)> job);
  void PostResult(std::function<void()> result);
  void WorkerLoop(Worker& worker);
  [[nodiscard]] auto OpenConnection() const -> MYSQL*;
  void RefreshConnection(MYSQL*& conn) const;

  auto ValidateSchema(MYSQL* conn, const Sha256Digest& storage_schema) const -> Result<void>;
  auto EnsureSchema(MYSQL* conn, const Sha256Digest& storage_schema) const -> Result<void>;
  auto EnsureSchemaLocked(MYSQL* conn, const Sha256Digest& storage_schema) const -> Result<void>;
  static auto QueryScalar(MYSQL* conn, std::string_view sql) -> Result<std::optional<std::string>>;
  static auto ExecSql(MYSQL* conn, std::string_view sql) -> Result<void>;
  static auto QueryRow(MYSQL* conn, std::string_view sql) -> Result<EntityRow>;
  static auto ReadRow(char** row, const unsigned long* lengths) -> EntityRow;
  static auto FetchByDbid(MYSQL* conn, DatabaseID dbid, uint16_t type_id, bool for_update = false)
      -> Result<EntityRow>;
  static auto FetchByName(MYSQL* conn, uint16_t type_id, std::string_view identifier,
                          bool for_update = false) -> Result<EntityRow>;
  static auto MysqlErr(MYSQL* conn, std::string_view prefix) -> Error;
  static auto ConstraintErrorKind(MYSQL* conn) -> DatabaseErrorKind;
  // Binary/identifier values go into SQL as X'..' hex literals: byte-exact and
  // injection-proof without escaping. Empty identifier maps to NULL.
  static auto HexLiteral(std::span<const std::byte> data) -> std::string;
  static auto IdentifierLiteral(std::string_view identifier) -> std::string;
  static auto BeginTxn(MYSQL* conn) -> Result<void>;
  static auto CheckCheckoutProcess(MYSQL* conn, const CheckoutInfo& owner) -> Result<void>;
  static auto Commit(MYSQL* conn) -> Result<void>;
  static void Rollback(MYSQL* conn);
  static auto LockOperationReceipt(MYSQL* conn, const std::array<std::byte, 16>& id,
                                   const Sha256Digest& hash)
      -> Result<std::optional<DatabaseOperationReceipt>>;
  static auto SaveOperationReceipt(MYSQL* conn, const std::array<std::byte, 16>& id,
                                   const DatabaseOperationReceipt& receipt) -> Result<void>;
  static auto DoCheckoutWithOperation(MYSQL* conn, const DatabaseCheckoutOperation& operation,
                                      bool acquire) -> GetResult;

  // write_password preserves the stored credential for ordinary entity updates.
  static auto DoPut(MYSQL* conn, DatabaseID dbid, uint16_t type_id, WriteFlags flags,
                    std::span<const std::byte> blob, const std::string& identifier,
                    bool write_password, const std::string& password_hash,
                    bool transaction_open = false) -> PutResult;
  static auto DoPutWithOperation(MYSQL* conn, const DatabaseWriteOperation& operation,
                                 DatabaseID dbid, uint16_t type_id, WriteFlags flags,
                                 std::span<const std::byte> blob, const std::string& identifier)
      -> PutResult;
  static auto DoGet(MYSQL* conn, DatabaseID dbid, uint16_t type_id) -> GetResult;
  static auto DoDel(MYSQL* conn, DatabaseID dbid, uint16_t type_id, bool transaction_open = false)
      -> DelResult;
  static auto DoLookup(MYSQL* conn, uint16_t type_id, const std::string& identifier)
      -> LookupResult;
  static auto DoCheckout(MYSQL* conn, DatabaseID dbid, uint16_t type_id,
                         const CheckoutInfo& new_owner) -> GetResult;
  static auto DoCheckoutByName(MYSQL* conn, uint16_t type_id, const std::string& identifier,
                               const CheckoutInfo& new_owner) -> GetResult;
  static auto AcquireCheckout(MYSQL* conn, DatabaseID dbid, uint16_t type_id,
                              const CheckoutInfo& owner, int64_t now_ms) -> Result<uint64_t>;
  static auto CheckoutTail(MYSQL* conn, DatabaseID dbid, uint16_t type_id, EntityRow row,
                           const CheckoutInfo& new_owner, int64_t now_ms) -> GetResult;
  static auto DoClearCheckout(MYSQL* conn, DatabaseID dbid, uint16_t type_id) -> Result<bool>;
  static auto DoClearForAddress(MYSQL* conn, const Address& base_addr) -> Result<int>;
  static auto DoGetAutoLoad(MYSQL* conn) -> Result<std::vector<EntityData>>;
  static auto DoSetAutoLoad(MYSQL* conn, DatabaseID dbid, uint16_t type_id, bool auto_load)
      -> Result<void>;
  static auto DoGetMaxDbid(MYSQL* conn, DatabaseID low, DatabaseID high) -> DbidRangeResult;
  static auto DoReserveEntityIds(MYSQL* conn, uint32_t count) -> Result<EntityIdRange>;
  static auto DoReadAccountSession(MYSQL* conn, DatabaseID dbid, uint16_t type_id,
                                   bool for_update = false) -> Result<AccountSession>;
  static auto DoUpdateAccountSession(MYSQL* conn, const AccountSessionOperation& operation)
      -> Result<AccountSession>;

  MYSQL* conn_{nullptr};  // bootstrap + inline (non-deferred) connection
  const EntityDefRegistry* entity_defs_{nullptr};
  DatabaseConfig config_;
  Sha256Digest storage_schema_{};
  bool started_{false};
  bool stopping_{false};
  bool deferred_mode_{false};
  std::atomic<std::size_t> pending_operations_{0};

  std::vector<std::unique_ptr<Worker>> workers_;

  std::mutex results_mutex_;
  std::deque<std::function<void()>> results_;

  static constexpr int kMaxCallbacksPerTick = 2048;
};

}  // namespace atlas

#endif  // ATLAS_LIB_DB_MYSQL_MYSQL_DATABASE_H_
