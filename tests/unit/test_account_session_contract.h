#ifndef ATLAS_TESTS_UNIT_TEST_ACCOUNT_SESSION_CONTRACT_H_
#define ATLAS_TESTS_UNIT_TEST_ACCOUNT_SESSION_CONTRACT_H_

#include <cstring>

#include <gtest/gtest.h>

#include "db/account_session.h"

namespace atlas::test_support {

inline auto UpdateSession(IDatabase& db, const AccountSessionOperation& operation)
    -> Result<AccountSession> {
  Result<AccountSession> result{Error{ErrorCode::kInternalError, "callback did not run"}};
  db.UpdateAccountSession(operation,
                          [&](Result<AccountSession> value) { result = std::move(value); });
  return result;
}

inline auto CreateSessionAccount(IDatabase& db, EntityID checkout_entity_id = 100)
    -> Result<AccountSessionOperation> {
  constexpr DatabaseID kAccount = 10000;
  PutResult created;
  db.PutEntity(kAccount, 1, WriteFlags::kCreateNew | WriteFlags::kExplicitDbid, {},
               "session-contract-account", [&](PutResult value) { created = std::move(value); });
  if (!created.success) return Error{ErrorCode::kIoError, created.error};
  CheckoutInfo checkout{Address(0x7F000001u, 31000), 7, checkout_entity_id, 0, 101};
  GetResult acquired;
  db.CheckoutEntity(kAccount, 1, checkout, [&](GetResult value) { acquired = std::move(value); });
  if (!acquired.success) return Error{ErrorCode::kIoError, acquired.error};
  checkout.ownership_version = acquired.checkout_version;
  if (checkout.ownership_version == 0)
    return Error{ErrorCode::kInternalError, "missing session account checkout version"};
  const AccountSessionOwner source{checkout.base_addr, 100, checkout.process_instance};
  AccountSessionOperation claim;
  claim.id[0] = std::byte{1};
  claim.account_dbid = kAccount;
  claim.account_type_id = 1;
  claim.actor = source;
  claim.claim_checkout = checkout;
  return claim;
}

inline void CheckAccountSessionContract(IDatabase& db) {
  auto seeded = CreateSessionAccount(db);
  ASSERT_TRUE(seeded.HasValue()) << seeded.Error().Message();
  const auto checkout = *seeded->claim_checkout;
  const auto source = seeded->actor;
  const auto kAccount = seeded->account_dbid;
  const AccountSessionOwner local{checkout.base_addr, 101, checkout.process_instance};
  const AccountSessionOwner target{Address(0x7F000001u, 32000), 200, 202};
  uint64_t serial = 1;
  auto operation = [&](AccountSessionAction action, uint64_t version,
                       const AccountSessionOwner& actor) {
    AccountSessionOperation op;
    ++serial;
    std::memcpy(op.id.data(), &serial, sizeof(serial));
    op.account_dbid = kAccount;
    op.account_type_id = 1;
    op.action = action;
    op.expected_version = version;
    op.actor = actor;
    return op;
  };
  Result<AccountSession> initial{Error{ErrorCode::kInternalError}};
  db.ReadAccountSession(kAccount, 1,
                        [&](Result<AccountSession> value) { initial = std::move(value); });
  ASSERT_TRUE(initial.HasValue());
  EXPECT_EQ(initial->version, 0u);
  auto claim = *seeded;
  auto active = UpdateSession(db, claim);
  ASSERT_TRUE(active.HasValue()) << active.Error().Message();
  EXPECT_EQ(active->owner, source);
  EXPECT_EQ(active->phase, AccountSessionPhase::kActive);
  auto replay = UpdateSession(db, claim);
  ASSERT_TRUE(replay.HasValue());
  EXPECT_EQ(*replay, *active);
  auto reused = claim;
  reused.expected_version = 1;
  auto conflict = UpdateSession(db, reused);
  ASSERT_FALSE(conflict.HasValue());
  EXPECT_EQ(conflict.Error().Code(), ErrorCode::kAlreadyExists);

  auto move = operation(AccountSessionAction::kMoveLocal, active->version, source);
  move.target = local;
  auto moved = UpdateSession(db, move);
  ASSERT_TRUE(moved.HasValue());
  EXPECT_EQ(moved->owner, local);
  auto stale = operation(AccountSessionAction::kRelease, active->version, source);
  EXPECT_FALSE(UpdateSession(db, stale).HasValue());
  auto prepare = operation(AccountSessionAction::kPrepare, moved->version, local);
  prepare.target = target;
  prepare.handoff_id = prepare.id;
  prepare.credential_hash[0] = std::byte{1};
  auto preparing = UpdateSession(db, prepare);
  ASSERT_TRUE(preparing.HasValue());
  EXPECT_EQ(preparing->owner, local);
  auto premature = operation(AccountSessionAction::kComplete, preparing->version, target);
  premature.handoff_id = prepare.handoff_id;
  premature.credential_hash = prepare.credential_hash;
  EXPECT_FALSE(UpdateSession(db, premature).HasValue());
  auto accept = operation(AccountSessionAction::kAccept, preparing->version, target);
  accept.handoff_id = prepare.handoff_id;
  auto accepted = UpdateSession(db, accept);
  ASSERT_TRUE(accepted.HasValue());
  auto suspend = operation(AccountSessionAction::kAwaitClient, accepted->version, local);
  suspend.handoff_id = prepare.handoff_id;
  auto awaiting = UpdateSession(db, suspend);
  ASSERT_TRUE(awaiting.HasValue());
  EXPECT_EQ(awaiting->phase, AccountSessionPhase::kAwaitingClient);
  auto complete = operation(AccountSessionAction::kComplete, awaiting->version, target);
  complete.handoff_id = prepare.handoff_id;
  auto bad_credential = complete;
  EXPECT_FALSE(UpdateSession(db, bad_credential).HasValue());
  complete.credential_hash = prepare.credential_hash;
  auto transferred = UpdateSession(db, complete);
  ASSERT_TRUE(transferred.HasValue());
  EXPECT_EQ(transferred->phase, AccountSessionPhase::kActive);
  EXPECT_EQ(transferred->owner, target);
  replay = UpdateSession(db, complete);
  ASSERT_TRUE(replay.HasValue());
  EXPECT_EQ(*replay, *transferred);
  auto abort = operation(AccountSessionAction::kAbort, awaiting->version, local);
  abort.handoff_id = prepare.handoff_id;
  EXPECT_FALSE(UpdateSession(db, abort).HasValue());
  auto release = operation(AccountSessionAction::kRelease, transferred->version, target);
  auto offline = UpdateSession(db, release);
  ASSERT_TRUE(offline.HasValue());
  EXPECT_EQ(offline->phase, AccountSessionPhase::kOffline);
  EXPECT_GT(offline->version, transferred->version);
  EXPECT_FALSE(UpdateSession(db, complete).HasValue());
  claim = operation(AccountSessionAction::kClaim, offline->version, source);
  claim.claim_checkout = checkout;
  active = UpdateSession(db, claim);
  ASSERT_TRUE(active.HasValue());
  EXPECT_GT(active->version, offline->version);
  EXPECT_FALSE(UpdateSession(db, release).HasValue());
  GetResult stored;
  db.GetEntity(kAccount, 1, [&](GetResult value) { stored = std::move(value); });
  ASSERT_TRUE(stored.success);
  EXPECT_EQ(stored.checked_out_by, checkout);
}

inline void CheckRetiredSessionReplay(IDatabase& db) {
  auto claim = CreateSessionAccount(db);
  ASSERT_TRUE(claim.HasValue());
  auto active = UpdateSession(db, *claim);
  ASSERT_TRUE(active.HasValue());
  Result<int> retired{Error{ErrorCode::kInternalError}};
  db.RetireCheckoutProcess(claim->actor.base_addr, claim->actor.process_instance,
                           [&](Result<int> value) { retired = std::move(value); });
  ASSERT_TRUE(retired.HasValue());
  auto replay = UpdateSession(db, *claim);
  ASSERT_FALSE(replay.HasValue());
  EXPECT_EQ(replay.Error().Code(), ErrorCode::kPermissionDenied);
}

inline void CheckSessionDeletionFence(IDatabase& db) {
  auto claim = CreateSessionAccount(db);
  ASSERT_TRUE(claim.HasValue());
  auto active = UpdateSession(db, *claim);
  ASSERT_TRUE(active.HasValue());
  DelResult removed;
  db.DelEntity(claim->account_dbid, 1, [&](DelResult value) { removed = std::move(value); });
  EXPECT_FALSE(removed.success);
  DatabaseWriteOperation deletion;
  deletion.id[0] = std::byte{20};
  deletion.requested_dbid = claim->account_dbid;
  deletion.requested_flags = WriteFlags::kDelete;
  deletion.expected_owner = claim->claim_checkout;
  PutResult deleted;
  db.PutEntityWithOperation(deletion, claim->account_dbid, 1, WriteFlags::kDelete, {}, "",
                            [&](PutResult value) { deleted = std::move(value); });
  EXPECT_FALSE(deleted.success);
  auto release = *claim;
  release.id[0] = std::byte{2};
  release.claim_checkout.reset();
  release.action = AccountSessionAction::kRelease;
  release.expected_version = active->version;
  auto offline = UpdateSession(db, release);
  ASSERT_TRUE(offline.HasValue());
  db.PutEntityWithOperation(deletion, claim->account_dbid, 1, WriteFlags::kDelete, {}, "",
                            [&](PutResult value) { deleted = std::move(value); });
  ASSERT_TRUE(deleted.success) << deleted.error;
  db.ReadAccountSession(claim->account_dbid, 1, [&](Result<AccountSession> value) {
    ASSERT_TRUE(value.HasValue());
    EXPECT_EQ(*value, *offline);
  });
  EXPECT_FALSE(UpdateSession(db, *claim).HasValue());
}

inline void CheckSessionAbortContract(IDatabase& db) {
  auto claim = CreateSessionAccount(db);
  ASSERT_TRUE(claim.HasValue());
  auto active = UpdateSession(db, *claim);
  ASSERT_TRUE(active.HasValue());
  auto prepare = *claim;
  prepare.id[0] = std::byte{2};
  prepare.claim_checkout.reset();
  prepare.action = AccountSessionAction::kPrepare;
  prepare.expected_version = active->version;
  prepare.target = {Address(0x7F000001u, 32000), 200, 202};
  prepare.handoff_id = prepare.id;
  prepare.credential_hash[0] = std::byte{1};
  auto preparing = UpdateSession(db, prepare);
  ASSERT_TRUE(preparing.HasValue());
  auto accept = prepare;
  accept.id[0] = std::byte{3};
  accept.action = AccountSessionAction::kAccept;
  accept.actor = prepare.target;
  accept.expected_version = preparing->version;
  auto accepted = UpdateSession(db, accept);
  ASSERT_TRUE(accepted.HasValue());
  auto suspend = prepare;
  suspend.id[0] = std::byte{4};
  suspend.action = AccountSessionAction::kAwaitClient;
  suspend.expected_version = accepted->version;
  auto awaiting = UpdateSession(db, suspend);
  ASSERT_TRUE(awaiting.HasValue());
  auto abort = prepare;
  abort.id[0] = std::byte{5};
  abort.action = AccountSessionAction::kAbort;
  abort.expected_version = awaiting->version;
  auto restored = UpdateSession(db, abort);
  ASSERT_TRUE(restored.HasValue());
  EXPECT_EQ(restored->owner, claim->actor);
  EXPECT_EQ(restored->phase, AccountSessionPhase::kActive);
  EXPECT_EQ(restored->target, AccountSessionOwner{});
  auto late = accept;
  late.id[0] = std::byte{6};
  late.action = AccountSessionAction::kComplete;
  late.expected_version = awaiting->version;
  EXPECT_FALSE(UpdateSession(db, late).HasValue());
}

}  // namespace atlas::test_support

#endif  // ATLAS_TESTS_UNIT_TEST_ACCOUNT_SESSION_CONTRACT_H_
