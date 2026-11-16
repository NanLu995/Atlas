#ifndef ATLAS_LIB_DB_ACCOUNT_SESSION_H_
#define ATLAS_LIB_DB_ACCOUNT_SESSION_H_

#include <algorithm>
#include <limits>

#include "db/write_operation.h"

namespace atlas {

inline constexpr int64_t kSessionRecoveryLifetimeMs = 10 * 60 * 1000;

inline void WriteRecoveryGrant(BinaryWriter& writer, const AccountRecoveryGrant& grant) {
  writer.WriteBytes(grant.credential_hash);
  writer.Write(grant.entity_dbid);
  writer.Write(grant.entity_type_id);
  writer.Write(grant.expires_at_ms);
}

[[nodiscard]] inline auto ReadRecoveryGrant(BinaryReader& reader) -> Result<AccountRecoveryGrant> {
  auto hash = reader.ReadBytes(32);
  auto dbid = reader.Read<DatabaseID>();
  auto type = reader.Read<uint16_t>();
  auto expires = reader.Read<int64_t>();
  if (!hash || !dbid || !type || !expires)
    return Error{ErrorCode::kInvalidArgument, "truncated recovery grant"};
  AccountRecoveryGrant grant;
  std::ranges::copy(*hash, grant.credential_hash.begin());
  grant.entity_dbid = *dbid;
  grant.entity_type_id = *type;
  grant.expires_at_ms = *expires;
  return grant;
}

[[nodiscard]] inline auto IsValidSessionOwner(const AccountSessionOwner& owner) -> bool {
  return owner.base_addr.Ip() != 0 && owner.base_addr.Port() != 0 &&
         owner.entity_id != kInvalidEntityID && owner.process_instance != 0 &&
         owner.process_instance <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
}

inline void WriteSessionOwner(BinaryWriter& writer, const AccountSessionOwner& owner) {
  writer.Write(owner.base_addr.Ip());
  writer.Write(owner.base_addr.Port());
  writer.Write(owner.entity_id);
  writer.Write(owner.process_instance);
}

[[nodiscard]] inline auto ReadSessionOwner(BinaryReader& reader) -> Result<AccountSessionOwner> {
  auto ip = reader.Read<uint32_t>();
  auto port = reader.Read<uint16_t>();
  auto entity_id = reader.Read<EntityID>();
  auto instance = reader.Read<uint64_t>();
  if (!ip || !port || !entity_id || !instance)
    return Error{ErrorCode::kInvalidArgument, "truncated account session owner"};
  return AccountSessionOwner{Address(*ip, *port), *entity_id, *instance};
}

[[nodiscard]] inline auto HasSessionValue(std::span<const std::byte> bytes) -> bool {
  return std::ranges::any_of(bytes, [](std::byte value) { return value != std::byte{0}; });
}

[[nodiscard]] inline auto ValidateAccountSession(const AccountSession& session) -> Result<void> {
  if (HasSessionValue(session.recovery.credential_hash)) {
    if (session.version == 0 || session.recovery.entity_dbid < kInvalidDBID ||
        session.recovery.entity_type_id == 0 || session.recovery.expires_at_ms <= 0)
      return Error{ErrorCode::kInvalidArgument, "invalid recovery grant"};
  } else if (session.recovery != AccountRecoveryGrant{}) {
    return Error{ErrorCode::kInvalidArgument, "recovery grant without credential"};
  }
  if (session.version > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ||
      session.phase > AccountSessionPhase::kAwaitingClient)
    return Error{ErrorCode::kInvalidArgument, "invalid account session version or phase"};
  if (session.phase == AccountSessionPhase::kOffline) {
    if (session.owner != AccountSessionOwner{} || session.target != AccountSessionOwner{} ||
        HasSessionValue(session.handoff_id) || HasSessionValue(session.credential_hash))
      return Error{ErrorCode::kInvalidArgument, "offline account session contains a grant"};
    return {};
  }
  if (session.version == 0 || !IsValidSessionOwner(session.owner))
    return Error{ErrorCode::kInvalidArgument, "invalid active account session owner"};
  if (session.phase == AccountSessionPhase::kActive) {
    if (session.target != AccountSessionOwner{} || HasSessionValue(session.credential_hash))
      return Error{ErrorCode::kInvalidArgument, "active account session contains a candidate"};
  } else if (!IsValidSessionOwner(session.target) || session.target == session.owner ||
             !HasSessionValue(session.handoff_id) || !HasSessionValue(session.credential_hash)) {
    return Error{ErrorCode::kInvalidArgument, "invalid account session handoff"};
  }
  return {};
}

inline void WriteAccountSession(BinaryWriter& writer, const AccountSession& session) {
  writer.Write(uint8_t{2});
  writer.Write(session.version);
  writer.Write(static_cast<uint8_t>(session.phase));
  WriteSessionOwner(writer, session.owner);
  WriteSessionOwner(writer, session.target);
  writer.WriteBytes(session.handoff_id);
  writer.WriteBytes(session.credential_hash);
  WriteRecoveryGrant(writer, session.recovery);
}

[[nodiscard]] inline auto ReadAccountSession(BinaryReader& reader) -> Result<AccountSession> {
  auto format = reader.Read<uint8_t>();
  auto version = reader.Read<uint64_t>();
  auto phase = reader.Read<uint8_t>();
  auto owner = ReadSessionOwner(reader);
  auto target = ReadSessionOwner(reader);
  auto handoff = reader.ReadBytes(16);
  auto hash = reader.ReadBytes(32);
  auto recovery = ReadRecoveryGrant(reader);
  if (!format || *format != 2 || !version || !phase || !owner || !target || !handoff || !hash ||
      !recovery)
    return Error{ErrorCode::kInvalidArgument, "invalid or truncated account session"};
  AccountSession session;
  session.version = *version;
  session.phase = static_cast<AccountSessionPhase>(*phase);
  session.owner = *owner;
  session.target = *target;
  std::ranges::copy(*handoff, session.handoff_id.begin());
  std::ranges::copy(*hash, session.credential_hash.begin());
  session.recovery = *recovery;
  if (auto valid = ValidateAccountSession(session); !valid) return valid.Error();
  return session;
}

[[nodiscard]] inline auto ValidateSessionOperation(const AccountSessionOperation& op)
    -> Result<void> {
  if (op.expected_version >= static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
    return Error{ErrorCode::kOutOfRange, "account session ownership version exhausted"};
  if (!HasSessionValue(op.id) || op.account_dbid <= kInvalidDBID || op.account_type_id == 0 ||
      !IsValidSessionOwner(op.actor) || op.action > AccountSessionAction::kClaimRecovery)
    return Error{ErrorCode::kInvalidArgument, "invalid account session operation"};
  if (op.action == AccountSessionAction::kClaim ||
      op.action == AccountSessionAction::kClaimRecovery) {
    if (!op.claim_checkout || op.claim_checkout->ownership_version == 0 ||
        op.claim_checkout->base_addr != op.actor.base_addr ||
        (op.action == AccountSessionAction::kClaim && op.claim_checkout->entity_id != 0 &&
         op.claim_checkout->entity_id != op.actor.entity_id) ||
        op.claim_checkout->process_instance != op.actor.process_instance)
      return Error{ErrorCode::kPermissionDenied, "session claim requires account checkout"};
  } else if (op.claim_checkout) {
    return Error{ErrorCode::kInvalidArgument, "unexpected account checkout in session operation"};
  }
  if (op.action == AccountSessionAction::kPrepare &&
      (!IsValidSessionOwner(op.target) || op.target == op.actor ||
       !HasSessionValue(op.handoff_id) || !HasSessionValue(op.credential_hash)))
    return Error{ErrorCode::kInvalidArgument, "session handoff requires target and credential"};
  if (op.action == AccountSessionAction::kMoveLocal &&
      (!IsValidSessionOwner(op.target) || op.target.base_addr != op.actor.base_addr ||
       op.target.process_instance != op.actor.process_instance || op.target == op.actor))
    return Error{ErrorCode::kInvalidArgument, "local session move cannot change process"};
  if (op.action == AccountSessionAction::kIssueRecovery ||
      op.action == AccountSessionAction::kConsumeRecovery ||
      op.action == AccountSessionAction::kClaimRecovery) {
    if (!HasSessionValue(op.recovery.credential_hash) || op.recovery.entity_dbid < kInvalidDBID ||
        op.recovery.entity_type_id == 0 || op.recovery.expires_at_ms != 0)
      return Error{ErrorCode::kInvalidArgument, "invalid recovery issuance"};
  } else if (op.recovery != AccountRecoveryGrant{}) {
    return Error{ErrorCode::kInvalidArgument, "unexpected recovery grant"};
  }
  if ((op.action == AccountSessionAction::kConsumeRecovery ||
       op.action == AccountSessionAction::kClaimRecovery) != HasSessionValue(op.recovery_proof))
    return Error{ErrorCode::kInvalidArgument, "invalid recovery proof"};
  if (op.action == AccountSessionAction::kClaimRecovery) {
    if (op.recovery.entity_dbid <= kInvalidDBID || !op.recovery_checkout ||
        op.recovery_checkout->ownership_version == 0 ||
        op.recovery_checkout->base_addr != op.actor.base_addr ||
        (op.recovery_checkout->entity_id != 0 &&
         op.recovery_checkout->entity_id != op.actor.entity_id) ||
        op.recovery_checkout->process_instance != op.actor.process_instance)
      return Error{ErrorCode::kPermissionDenied, "recovery requires persistent target checkout"};
  } else if (op.recovery_checkout) {
    return Error{ErrorCode::kInvalidArgument, "unexpected recovery checkout"};
  }
  return {};
}

[[nodiscard]] inline auto ApplySessionOperation(const AccountSession& current,
                                                const AccountSessionOperation& op,
                                                int64_t now_ms = 0) -> Result<AccountSession> {
  if (auto valid = ValidateSessionOperation(op); !valid) return valid.Error();
  if (auto valid = ValidateAccountSession(current); !valid) return valid.Error();
  if (current.version != op.expected_version)
    return Error{ErrorCode::kPermissionDenied, "account session ownership version mismatch"};
  auto next = current;
  const bool owner = current.owner == op.actor;
  const bool target = current.target == op.actor;
  auto conflict = []() -> Result<AccountSession> {
    return Error{ErrorCode::kPermissionDenied, "account session owner or phase mismatch"};
  };
  switch (op.action) {
    case AccountSessionAction::kClaim:
      if (current.phase != AccountSessionPhase::kOffline) return conflict();
      next.owner = op.actor;
      next.recovery = {};
      next.phase = AccountSessionPhase::kActive;
      break;
    case AccountSessionAction::kPrepare:
      if (!owner || current.phase != AccountSessionPhase::kActive) return conflict();
      next.phase = AccountSessionPhase::kPreparing;
      next.target = op.target;
      next.handoff_id = op.handoff_id;
      next.credential_hash = op.credential_hash;
      break;
    case AccountSessionAction::kAccept:
      if (!target || current.phase != AccountSessionPhase::kPreparing ||
          current.handoff_id != op.handoff_id)
        return conflict();
      next.phase = AccountSessionPhase::kAccepted;
      break;
    case AccountSessionAction::kAwaitClient:
      if (!owner || current.phase != AccountSessionPhase::kAccepted ||
          current.handoff_id != op.handoff_id)
        return conflict();
      next.phase = AccountSessionPhase::kAwaitingClient;
      break;
    case AccountSessionAction::kComplete:
      if (!target || current.phase != AccountSessionPhase::kAwaitingClient ||
          current.handoff_id != op.handoff_id || current.credential_hash != op.credential_hash)
        return conflict();
      next.phase = AccountSessionPhase::kActive;
      next.owner = current.target;
      next.recovery = {};
      next.target = {};
      next.credential_hash = {};
      break;
    case AccountSessionAction::kAbort:
      if ((!owner && !target) || current.phase < AccountSessionPhase::kPreparing ||
          current.handoff_id != op.handoff_id)
        return conflict();
      next.phase = AccountSessionPhase::kActive;
      next.target = {};
      next.handoff_id = {};
      next.credential_hash = {};
      break;
    case AccountSessionAction::kRelease:
      if (!owner || current.phase == AccountSessionPhase::kOffline) return conflict();
      next = {};
      break;
    case AccountSessionAction::kMoveLocal:
      if (!owner || current.phase != AccountSessionPhase::kActive) return conflict();
      next.owner = op.target;
      next.recovery = {};
      next.handoff_id = {};
      break;
    case AccountSessionAction::kIssueRecovery:
    case AccountSessionAction::kConsumeRecovery:
    case AccountSessionAction::kClaimRecovery:
      if ((!owner && op.action != AccountSessionAction::kClaimRecovery) ||
          current.phase != AccountSessionPhase::kActive)
        return conflict();
      if (op.action != AccountSessionAction::kIssueRecovery &&
          (now_ms >= current.recovery.expires_at_ms ||
           !Sha256DigestEquals(current.recovery.credential_hash, op.recovery_proof) ||
           current.recovery.entity_dbid != op.recovery.entity_dbid ||
           current.recovery.entity_type_id != op.recovery.entity_type_id))
        return Error{ErrorCode::kPermissionDenied, "recovery credential invalid or expired"};
      if (Sha256DigestEquals(current.recovery.credential_hash, op.recovery.credential_hash))
        return Error{ErrorCode::kInvalidArgument, "recovery credential must change"};
      if (now_ms <= 0 || now_ms > std::numeric_limits<int64_t>::max() - kSessionRecoveryLifetimeMs)
        return Error{ErrorCode::kOutOfRange, "recovery expiry unavailable"};
      next.recovery = op.recovery;
      next.recovery.expires_at_ms = now_ms + kSessionRecoveryLifetimeMs;
      if (op.action == AccountSessionAction::kClaimRecovery) next.owner = op.actor;
      break;
    case AccountSessionAction::kRevokeRecovery:
      if (!owner || current.phase == AccountSessionPhase::kOffline) return conflict();
      next.recovery = {};
      break;
  }
  next.version = current.version + 1;
  if (auto valid = ValidateAccountSession(next); !valid) return valid.Error();
  return next;
}

inline void WriteSessionCheckout(BinaryWriter& writer, const std::optional<CheckoutInfo>& proof) {
  writer.Write(static_cast<uint8_t>(proof.has_value()));
  if (!proof) return;
  writer.Write(proof->base_addr.Ip());
  writer.Write(proof->base_addr.Port());
  writer.Write(proof->app_id);
  writer.Write(proof->entity_id);
  writer.Write(proof->ownership_version);
  writer.Write(proof->process_instance);
}

[[nodiscard]] inline auto ReadSessionCheckout(BinaryReader& reader)
    -> Result<std::optional<CheckoutInfo>> {
  auto present = reader.Read<uint8_t>();
  if (!present || *present > 1)
    return Error{ErrorCode::kInvalidArgument, "invalid account session checkout flag"};
  if (*present == 0) return std::optional<CheckoutInfo>{};
  auto ip = reader.Read<uint32_t>();
  auto port = reader.Read<uint16_t>();
  auto app = reader.Read<uint32_t>();
  auto entity = reader.Read<uint32_t>();
  auto ownership = reader.Read<uint64_t>();
  auto instance = reader.Read<uint64_t>();
  if (!ip || !port || !app || !entity || !ownership || !instance)
    return Error{ErrorCode::kInvalidArgument, "truncated account session checkout proof"};
  return std::optional<CheckoutInfo>{
      CheckoutInfo{Address(*ip, *port), *app, *entity, *ownership, *instance}};
}

inline void WriteSessionOperation(BinaryWriter& writer, const AccountSessionOperation& op) {
  writer.WriteBytes(op.id);
  writer.Write(op.account_dbid);
  writer.Write(op.account_type_id);
  writer.Write(op.expected_version);
  writer.Write(static_cast<uint8_t>(op.action));
  WriteSessionOwner(writer, op.actor);
  WriteSessionOwner(writer, op.target);
  writer.WriteBytes(op.handoff_id);
  writer.WriteBytes(op.credential_hash);
  WriteSessionCheckout(writer, op.claim_checkout);
  WriteRecoveryGrant(writer, op.recovery);
  writer.WriteBytes(op.recovery_proof);
  WriteSessionCheckout(writer, op.recovery_checkout);
}

[[nodiscard]] inline auto ReadSessionOperation(BinaryReader& reader)
    -> Result<AccountSessionOperation> {
  auto id = reader.ReadBytes(16);
  auto dbid = reader.Read<DatabaseID>();
  auto type = reader.Read<uint16_t>();
  auto version = reader.Read<uint64_t>();
  auto action = reader.Read<uint8_t>();
  auto actor = ReadSessionOwner(reader);
  auto target = ReadSessionOwner(reader);
  auto handoff = reader.ReadBytes(16);
  auto credential = reader.ReadBytes(32);
  auto checkout = ReadSessionCheckout(reader);
  if (!id || !dbid || !type || !version || !action || !actor || !target || !handoff ||
      !credential || !checkout)
    return Error{ErrorCode::kInvalidArgument, "truncated account session operation"};
  AccountSessionOperation op;
  std::ranges::copy(*id, op.id.begin());
  op.account_dbid = *dbid;
  op.account_type_id = *type;
  op.expected_version = *version;
  op.action = static_cast<AccountSessionAction>(*action);
  op.actor = *actor;
  op.target = *target;
  std::ranges::copy(*handoff, op.handoff_id.begin());
  std::ranges::copy(*credential, op.credential_hash.begin());
  op.claim_checkout = *checkout;
  auto recovery = ReadRecoveryGrant(reader);
  if (!recovery) return recovery.Error();
  op.recovery = *recovery;
  auto proof = reader.ReadBytes(32);
  if (!proof) return proof.Error();
  std::ranges::copy(*proof, op.recovery_proof.begin());
  auto recovery_checkout = ReadSessionCheckout(reader);
  if (!recovery_checkout) return recovery_checkout.Error();
  op.recovery_checkout = *recovery_checkout;
  return op;
}

[[nodiscard]] inline auto HashSessionOperation(const AccountSessionOperation& op)
    -> Result<Sha256Digest> {
  BinaryWriter writer;
  writer.Write(uint8_t{3});
  WriteSessionOperation(writer, op);
  return Sha256(writer.Data());
}

[[nodiscard]] inline auto ConfirmSessionReceipt(const AccountSessionOperation& op,
                                                const DatabaseOperationReceipt& receipt,
                                                AccountSession current) -> Result<AccountSession> {
  if (receipt.dbid != op.account_dbid || receipt.checkout_version == 0 ||
      receipt.checkout_version != op.expected_version + 1)
    return Error{ErrorCode::kCommitUnknown, "invalid account session receipt"};
  if (current.version != receipt.checkout_version)
    return Error{ErrorCode::kPermissionDenied, "account session operation was superseded"};
  return current;
}

}  // namespace atlas

#endif  // ATLAS_LIB_DB_ACCOUNT_SESSION_H_
