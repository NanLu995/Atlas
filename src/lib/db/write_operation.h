#ifndef ATLAS_LIB_DB_WRITE_OPERATION_H_
#define ATLAS_LIB_DB_WRITE_OPERATION_H_

#include "db/idatabase.h"
#include "platform/sha256.h"
#include "serialization/binary_stream.h"

namespace atlas {

struct DatabaseOperationReceipt {
  Sha256Digest request_hash{};
  DatabaseID dbid{kInvalidDBID};
  uint64_t checkout_version{0};
};

[[nodiscard]] inline auto HashWriteOperation(const DatabaseWriteOperation& operation,
                                             uint16_t type_id, std::span<const std::byte> blob,
                                             const std::string& identifier)
    -> Result<Sha256Digest> {
  BinaryWriter request;
  request.Write(uint8_t{1});
  request.Write(operation.requested_dbid);
  request.Write(static_cast<uint8_t>(operation.requested_flags));
  request.Write(operation.entity_id);
  request.Write(static_cast<uint8_t>(operation.expected_owner.has_value()));
  if (operation.expected_owner) {
    request.Write(operation.expected_owner->base_addr.Ip());
    request.Write(operation.expected_owner->base_addr.Port());
    request.Write(operation.expected_owner->app_id);
    request.Write(operation.expected_owner->entity_id);
    request.Write(operation.expected_owner->ownership_version);
    request.Write(operation.expected_owner->process_instance);
  }
  request.Write(static_cast<uint8_t>(operation.create_owner.has_value()));
  if (operation.create_owner) {
    request.Write(operation.create_owner->base_addr.Ip());
    request.Write(operation.create_owner->base_addr.Port());
    request.Write(operation.create_owner->app_id);
    request.Write(operation.create_owner->entity_id);
    request.Write(operation.create_owner->process_instance);
  }
  request.Write(type_id);
  request.WriteString(identifier);
  request.Write(static_cast<uint64_t>(blob.size()));
  request.WriteBytes(blob);
  return Sha256(request.Data());
}

[[nodiscard]] inline auto HashCheckoutOperation(const DatabaseCheckoutOperation& operation)
    -> Result<Sha256Digest> {
  BinaryWriter request;
  request.Write(uint8_t{2});
  request.Write(operation.dbid);
  request.Write(operation.type_id);
  request.WriteString(operation.identifier);
  request.Write(operation.owner.base_addr.Ip());
  request.Write(operation.owner.base_addr.Port());
  request.Write(operation.owner.app_id);
  request.Write(operation.owner.entity_id);
  request.Write(operation.owner.process_instance);
  return Sha256(request.Data());
}

[[nodiscard]] inline auto ValidateCheckoutReceipt(const DatabaseCheckoutOperation& operation,
                                                  const DatabaseOperationReceipt& receipt)
    -> Result<void> {
  if (receipt.checkout_version == 0 || receipt.dbid <= kInvalidDBID ||
      (operation.dbid != kInvalidDBID && receipt.dbid != operation.dbid))
    return Error{ErrorCode::kCommitUnknown, "invalid checkout receipt"};
  return {};
}

[[nodiscard]] inline auto ConfirmCheckoutReceipt(const DatabaseCheckoutOperation& operation,
                                                 const DatabaseOperationReceipt& receipt,
                                                 GetResult current) -> GetResult {
  if (auto valid = ValidateCheckoutReceipt(operation, receipt); !valid) {
    GetResult failed;
    failed.error = valid.Error().Message();
    failed.error_kind = DatabaseErrorKind::kCommitUnknown;
    return failed;
  }
  if (!current.success) return current;
  if (receipt.dbid != current.data.dbid || current.data.type_id != operation.type_id) {
    GetResult failed;
    failed.error = "invalid checkout receipt entity";
    failed.error_kind = DatabaseErrorKind::kCommitUnknown;
    return failed;
  }
  auto expected = operation.owner;
  expected.ownership_version = receipt.checkout_version;
  if (current.checked_out_by != expected) {
    GetResult failed;
    failed.error = "checkout_lease_expired";
    failed.error_kind = DatabaseErrorKind::kOwnershipConflict;
    failed.data.dbid = receipt.dbid;
    failed.checkout_version = receipt.checkout_version;
    return failed;
  }
  current.checked_out_by.reset();
  current.checkout_version = receipt.checkout_version;
  return current;
}

[[nodiscard]] inline auto RestoreWriteReceipt(const DatabaseWriteOperation& operation,
                                              const DatabaseOperationReceipt& receipt)
    -> PutResult {
  PutResult result;
  result.success = true;
  result.dbid = receipt.dbid;
  if (operation.create_owner) {
    result.checkout_owner = operation.create_owner;
    result.checkout_owner->ownership_version = receipt.checkout_version;
  }
  return result;
}

}  // namespace atlas

#endif  // ATLAS_LIB_DB_WRITE_OPERATION_H_
