#ifndef ATLAS_LIB_DB_IDATABASE_H_
#define ATLAS_LIB_DB_IDATABASE_H_

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "entitydef/entity_def_registry.h"
#include "foundation/error.h"
#include "network/address.h"
#include "server/entity_types.h"

namespace atlas {

struct DatabaseConfig {
  std::string type{"mysql"};  // "mysql" or explicitly selected "sqlite"

  std::filesystem::path sqlite_path{"data/atlas_dev.sqlite3"};
  bool sqlite_wal{true};
  int sqlite_busy_timeout_ms{5000};
  bool sqlite_foreign_keys{true};

  std::string mysql_host{"127.0.0.1"};
  uint16_t mysql_port{3306};
  std::string mysql_user;
  std::string mysql_password;
  std::string mysql_database;
  int mysql_pool_size{4};
};

enum class WriteFlags : uint8_t {
  kNone = 0,
  kCreateNew = 1 << 0,     // new entity; DB assigns ID unless kExplicitDbid is set
  kExplicitDbid = 1 << 1,  // use specified dbid for create/update
  kLogOff = 1 << 2,        // entity offline, clear checkout record
  kDelete = 1 << 3,        // delete from database
  kAutoLoadOn = 1 << 4,    // mark as auto-load
  kAutoLoadOff = 1 << 5,   // clear auto-load
  kCellData = 1 << 6,      // reserved: blob includes cell data
};

enum class DatabaseErrorKind : uint8_t {
  kNone = 0,
  kDuplicateDbid,
  kDuplicateIdentifier,
  kCommitUnknown,
  kNotFound,
  kOperationConflict,
  kOwnershipConflict,
};

[[nodiscard]] inline auto operator|(WriteFlags a, WriteFlags b) -> WriteFlags {
  return static_cast<WriteFlags>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}

[[nodiscard]] inline auto operator&(WriteFlags a, WriteFlags b) -> WriteFlags {
  return static_cast<WriteFlags>(static_cast<uint8_t>(a) & static_cast<uint8_t>(b));
}

[[nodiscard]] inline auto HasFlag(WriteFlags flags, WriteFlags f) -> bool {
  return (static_cast<uint8_t>(flags) & static_cast<uint8_t>(f)) != 0;
}

struct EntityData {
  DatabaseID dbid{kInvalidDBID};
  uint16_t type_id{0};
  std::vector<std::byte> blob;  // persistent attribute binary (C# SpanWriter output)
  std::string identifier;       // value of [Identifier] property (if any)
};

struct CheckoutInfo {
  Address base_addr;      // owning BaseApp internal address
  uint32_t app_id{0};     // owning BaseApp AppID (more stable than address)
  uint32_t entity_id{0};  // EntityID on the owning BaseApp
  uint64_t ownership_version{0};
  // Zero is reserved for direct backend operations without process death tracking.
  uint64_t process_instance{0};

  [[nodiscard]] auto operator==(const CheckoutInfo&) const -> bool = default;
};

struct DatabaseWriteOperation {
  std::array<std::byte, 16> id{};
  DatabaseID requested_dbid{kInvalidDBID};
  WriteFlags requested_flags{WriteFlags::kNone};
  uint32_t entity_id{0};
  std::optional<CheckoutInfo> expected_owner{};
  std::optional<CheckoutInfo> create_owner{};

  [[nodiscard]] auto IsValid() const -> bool {
    for (auto byte : id) {
      if (byte != std::byte{0}) return true;
    }
    return false;
  }
};

struct DatabaseCheckoutOperation {
  std::array<std::byte, 16> id{};
  DatabaseID dbid{kInvalidDBID};  // Zero selects identifier lookup.
  uint16_t type_id{0};
  std::string identifier;
  CheckoutInfo owner;

  [[nodiscard]] auto IsValid() const -> bool {
    if (type_id == 0 || dbid < 0 || (dbid == kInvalidDBID && identifier.empty()) ||
        owner.base_addr.Port() == 0 || owner.ownership_version != 0)
      return false;
    for (auto byte : id) {
      if (byte != std::byte{0}) return true;
    }
    return false;
  }
};

struct PutResult {
  bool success{false};
  DatabaseID dbid{kInvalidDBID};  // assigned or existing DBID
  std::string error;
  DatabaseErrorKind error_kind{DatabaseErrorKind::kNone};
  std::optional<CheckoutInfo> checkout_owner{};
};

struct GetResult {
  bool success{false};
  EntityData data;
  std::optional<CheckoutInfo> checked_out_by;  // non-empty if already checked out
  std::string error;
  DatabaseErrorKind error_kind{DatabaseErrorKind::kNone};
  uint64_t checkout_version{0};
};

struct DelResult {
  bool success{false};
  std::string error;
};

struct LookupResult {
  bool found{false};
  DatabaseID dbid{kInvalidDBID};
  std::string password_hash;  // sm_passwordHash (may be empty)
  std::string error;          // non-empty on backend failure (distinct from not-found)
};

struct DbidRangeResult {
  bool success{false};
  DatabaseID max_dbid{kInvalidDBID};
  std::string error;
};

struct EntityIdRange {
  EntityID first{kInvalidEntityID};
  EntityID last{kInvalidEntityID};
};

class IDatabase {
 public:
  virtual ~IDatabase() = default;

  IDatabase(const IDatabase&) = delete;
  IDatabase& operator=(const IDatabase&) = delete;

  [[nodiscard]] virtual auto Startup(const DatabaseConfig& config,
                                     const EntityDefRegistry& entity_defs) -> Result<void> = 0;

  virtual void Shutdown() = 0;

  virtual void PutEntity(DatabaseID dbid, uint16_t type_id, WriteFlags flags,
                         std::span<const std::byte> blob, const std::string& identifier,
                         std::function<void(PutResult)> callback) = 0;

  virtual void PutEntityWithOperation(const DatabaseWriteOperation& /*operation*/, DatabaseID dbid,
                                      uint16_t /*type_id*/, WriteFlags /*flags*/,
                                      std::span<const std::byte> /*blob*/,
                                      const std::string& /*identifier*/,
                                      std::function<void(PutResult)> callback) {
    callback(PutResult{false, dbid, "write_operation_not_supported"});
  }

  virtual void PutEntityWithPassword(DatabaseID dbid, uint16_t type_id, WriteFlags flags,
                                     std::span<const std::byte> blob, const std::string& identifier,
                                     const std::string& /*password_hash*/,
                                     std::function<void(PutResult)> callback) {
    PutEntity(dbid, type_id, flags, blob, identifier, std::move(callback));
  }

  virtual void GetEntity(DatabaseID dbid, uint16_t type_id,
                         std::function<void(GetResult)> callback) = 0;

  virtual void DelEntity(DatabaseID dbid, uint16_t type_id,
                         std::function<void(DelResult)> callback) = 0;

  virtual void LookupByName(uint16_t type_id, const std::string& identifier,
                            std::function<void(LookupResult)> callback) = 0;

  virtual void CheckoutEntity(DatabaseID dbid, uint16_t type_id, const CheckoutInfo& new_owner,
                              std::function<void(GetResult)> callback) = 0;

  virtual void CheckoutEntityByName(uint16_t type_id, const std::string& identifier,
                                    const CheckoutInfo& new_owner,
                                    std::function<void(GetResult)> callback) = 0;

  virtual void CheckoutWithOperation(const DatabaseCheckoutOperation& /*operation*/,
                                     std::function<void(GetResult)> callback) {
    GetResult result;
    result.error = "checkout_operation_not_supported";
    callback(std::move(result));
  }

  virtual void ResolveCheckoutOperation(const DatabaseCheckoutOperation& /*operation*/,
                                        std::function<void(GetResult)> callback) {
    GetResult result;
    result.error = "checkout_resolution_not_supported";
    callback(std::move(result));
  }

  virtual void ReleaseCheckout(DatabaseID /*dbid*/, uint16_t /*type_id*/,
                               const CheckoutInfo& /*owner*/,
                               std::function<void(Result<void>)> callback) {
    callback(Error{ErrorCode::kNotSupported, "conditional checkout release is not supported"});
  }

  virtual void ClearCheckout(DatabaseID dbid, uint16_t type_id,
                             std::function<void(bool)> callback) = 0;

  virtual void ClearCheckoutsForAddress(const Address& base_addr,
                                        std::function<void(int cleared_count)> callback) = 0;

  virtual void RetireCheckoutProcess(const Address& /*base_addr*/, uint64_t /*instance*/,
                                     std::function<void(Result<int>)> callback) {
    callback(Error{ErrorCode::kNotSupported, "process-fenced checkout retirement unsupported"});
  }

  virtual void MarkCheckoutCleared(DatabaseID dbid, uint16_t type_id) {
    ClearCheckout(dbid, type_id, [](bool) {});
  }

  virtual void GetAutoLoadEntities(std::function<void(std::vector<EntityData>)> callback) = 0;

  virtual void SetAutoLoad(DatabaseID dbid, uint16_t type_id, bool auto_load) = 0;

  // Includes retained operation receipts and lease history; deletion does not erase used IDs.
  virtual void GetMaxDbidInRange(DatabaseID low, DatabaseID high,
                                 std::function<void(DbidRangeResult)> callback) = 0;

  virtual void ReserveEntityIds(uint32_t count,
                                std::function<void(Result<EntityIdRange>)> callback) = 0;

  virtual void SetDeferredMode(bool /*enabled*/) {}

  virtual void ProcessResults() = 0;

  virtual void BeginBatch() {}

  virtual void EndBatch() {}

  [[nodiscard]] virtual auto SupportsMultiDbapp() const -> bool { return false; }

 protected:
  IDatabase() = default;
};

}  // namespace atlas

#endif  // ATLAS_LIB_DB_IDATABASE_H_
