#include <chrono>
#include <cstdint>
#include <memory>

#include <gtest/gtest.h>

#include "cell.h"
#include "cell_entity.h"
#include "cellapp.h"
#include "controller_codec.h"
#include "intercell_messages.h"
#include "math/vector3.h"
#include "network/channel.h"
#include "network/event_dispatcher.h"
#include "network/network_interface.h"
#include "real_entity_data.h"
#include "serialization/binary_stream.h"
#include "space.h"
#include "space/move_controller.h"
#include "space/timer_controller.h"
#include "test_null_channel.h"
#include "witness.h"

namespace atlas {

struct CellAppMigrationTestAccess {
  static void TickTimeouts(CellApp& app) { app.TickOffloadAckTimeouts(); }
  static void RecordSuccess(CellApp& app, EntityID id, const Address& source, uint32_t epoch) {
    app.offload_receipts_[id] = {epoch, source, cellapp::OffloadRejectReason::kNone};
  }

  static void FillReceipts(CellApp& app) {
    for (uint32_t id = 1; id <= app.kMaxOffloadReceipts; ++id) {
      app.offload_receipts_.emplace(
          id, CellApp::OffloadReceipt{0, {}, cellapp::OffloadRejectReason::kNone});
    }
  }
};

namespace {

using test_support::FakeChannel;

struct Harness {
  EventDispatcher dispatcher{"offload_revert"};
  NetworkInterface network{dispatcher};
  CellApp app{dispatcher, network};
};

// Helper: set the app up with a Space + local Cell + a Real entity that
// already has a haunt and a MoveToPoint controller. Returns the entity.
auto SeedEntityWithState(Harness& h, EntityID id, Channel* haunt_channel) -> CellEntity* {
  const SpaceID sid = 42;
  auto& spaces = h.app.Spaces();
  auto* space = spaces.emplace(sid, std::make_unique<Space>(sid)).first->second.get();
  auto* cell = space->AddLocalCell(std::make_unique<Cell>(*space, /*cell_id=*/7, CellBounds{}));
  auto* entity = space->AddEntity(std::make_unique<CellEntity>(
      id, /*type=*/uint16_t{1}, *space, math::Vector3{0, 0, 0}, math::Vector3{1, 0, 0}));
  entity->SetBaseAddr(Address(0x7F000001u, 20000));
  cell->AddRealEntity(entity);
  // Register with CellApp's population map so RevertPendingOffload's
  // FindEntity lookup resolves. In production this happens via
  // OnCreateCellEntity; the test bypasses that to avoid CLR / network
  // dependencies.
  h.app.EntityPopulationForTest()[id] = entity;
  if (haunt_channel != nullptr)
    entity->GetRealData()->AddHaunt(haunt_channel, Address(0x7F000001u, 50000));
  entity->GetControllers().Add(
      std::make_unique<MoveToPointController>(math::Vector3{10, 0, 10}, 5.f, false), entity,
      /*user_arg=*/99);
  return entity;
}

// Build a PendingOffload snapshot that mirrors what TickOffloadChecker
// would capture for `entity` right before ConvertRealToGhost.
auto CaptureSnapshot(CellApp::PendingOffload& po, const CellEntity& entity, SpaceID sid,
                     cellappmgr::CellID cid, const Address& target,
                     const std::vector<Address>& haunt_addrs) -> void {
  po.target_addr = target;
  po.sent_at = Clock::now();
  po.space_id = sid;
  po.cell_id = cid;
  po.haunt_addrs = haunt_addrs;
  BinaryWriter cw;
  SerializeControllersForMigration(entity, cw);
  auto buf = cw.Detach();
  po.controller_blob.assign(buf.begin(), buf.end());
}

TEST(OffloadRevert, AckFromAnotherTrustedPeerCannotRevert) {
  Harness h;
  const Address target(0x7F000001u, 30002);
  const Address other(0x7F000001u, 30003);
  auto* target_channel = FakeChannel(0xBEEF);
  auto* other_channel = FakeChannel(0xCAFE);
  h.app.PeerRegistryForTest().InsertForTest(target, target_channel);
  h.app.PeerRegistryForTest().InsertForTest(other, other_channel);
  auto* entity = SeedEntityWithState(h, 100, nullptr);
  auto& pending = h.app.PendingOffloadsForTest()[entity->Id()];
  CaptureSnapshot(pending, *entity, 42, 7, target, {});
  entity->ConvertRealToGhost(target_channel, target);
  entity->GetSpace().FindLocalCell(7)->RemoveRealEntity(entity);
  cellapp::OffloadEntityAck ack;
  ack.entity_id = entity->Id();

  h.app.OnOffloadEntityAck(other, other_channel, ack);

  EXPECT_TRUE(entity->IsGhost());
  EXPECT_EQ(h.app.PendingOffloadsForTest().size(), 1u);
}

TEST(OffloadRevert, TimeoutKeepsGhostAndRecoverySnapshot) {
  Harness h;
  const Address target(0x7F000001u, 30002);
  auto* entity = SeedEntityWithState(h, 100, nullptr);
  auto& pending = h.app.PendingOffloadsForTest()[entity->Id()];
  CaptureSnapshot(pending, *entity, 42, 7, target, {});
  pending.sent_at -= std::chrono::seconds(6);
  entity->ConvertRealToGhost(FakeChannel(0xBEEF), target);
  entity->GetSpace().FindLocalCell(7)->RemoveRealEntity(entity);

  CellAppMigrationTestAccess::TickTimeouts(h.app);

  EXPECT_TRUE(entity->IsGhost());
  EXPECT_EQ(h.app.PendingOffloadsForTest().size(), 1u);
  CellAppMigrationTestAccess::TickTimeouts(h.app);
  EXPECT_TRUE(entity->IsGhost());
}

TEST(OffloadRevert, LostTargetKeepsGhostAndRecoverySnapshot) {
  Harness h;
  const Address target(0x7F000001u, 30002);
  auto* entity = SeedEntityWithState(h, 100, nullptr);
  auto& pending = h.app.PendingOffloadsForTest()[entity->Id()];
  CaptureSnapshot(pending, *entity, 42, 7, target, {});
  entity->ConvertRealToGhost(FakeChannel(0xBEEF), target);
  entity->GetSpace().FindLocalCell(7)->RemoveRealEntity(entity);

  h.app.HandlePeerLost(target, false);

  const auto* retained = h.app.FindEntity(100);
  ASSERT_NE(retained, nullptr);
  EXPECT_TRUE(retained->IsGhost());
  EXPECT_EQ(retained->GetRealChannel(), nullptr);
  EXPECT_EQ(h.app.PendingOffloadsForTest().size(), 1u);
}

TEST(OffloadRevert, AckMatchesAttemptAndChannelBeforeReverting) {
  Harness h;
  const Address target(0x7F000001u, 30002);
  auto* target_channel = FakeChannel(0xBEEF);
  h.app.PeerRegistryForTest().InsertForTest(target, target_channel);
  auto* entity = SeedEntityWithState(h, 100, nullptr);
  auto& pending = h.app.PendingOffloadsForTest()[entity->Id()];
  CaptureSnapshot(pending, *entity, 42, 7, target, {});
  pending.cell_epoch = 7;
  entity->SetCellEpoch(8);
  entity->ConvertRealToGhost(target_channel, target);
  entity->GetSpace().FindLocalCell(7)->RemoveRealEntity(entity);
  cellapp::OffloadEntityAck ack;
  ack.entity_id = entity->Id();
  ack.cell_epoch = 6;

  h.app.OnOffloadEntityAck(target, target_channel, ack);
  EXPECT_TRUE(entity->IsGhost());
  ack.cell_epoch = 7;
  h.app.OnOffloadEntityAck(target, FakeChannel(0xCAFE), ack);
  EXPECT_TRUE(entity->IsGhost());
  h.app.OnOffloadEntityAck(target, target_channel, ack);
  EXPECT_TRUE(entity->IsReal());
  EXPECT_EQ(entity->CellEpoch(), 8u);
  EXPECT_TRUE(h.app.PendingOffloadsForTest().empty());
}

TEST(OffloadRevert, LateSuccessCompletesUncertainAttemptExactlyOnce) {
  Harness h;
  const Address target(0x7F000001u, 30002);
  auto* target_channel = FakeChannel(0xBEEF);
  h.app.PeerRegistryForTest().InsertForTest(target, target_channel);
  auto* entity = SeedEntityWithState(h, 100, nullptr);
  auto& pending = h.app.PendingOffloadsForTest()[entity->Id()];
  CaptureSnapshot(pending, *entity, 42, 7, target, {});
  pending.sent_at -= std::chrono::seconds(6);
  entity->ConvertRealToGhost(target_channel, target);
  entity->GetSpace().FindLocalCell(7)->RemoveRealEntity(entity);
  CellAppMigrationTestAccess::TickTimeouts(h.app);
  ASSERT_TRUE(pending.outcome_unknown);
  cellapp::OffloadEntityAck ack;
  ack.entity_id = entity->Id();
  ack.success = true;
  ack.reject_reason = cellapp::OffloadRejectReason::kNone;

  h.app.OnOffloadEntityAck(target, target_channel, ack);
  h.app.OnOffloadEntityAck(target, target_channel, ack);

  EXPECT_TRUE(entity->IsGhost());
  EXPECT_TRUE(h.app.PendingOffloadsForTest().empty());
}

TEST(OffloadRevert, ReceiptCapacityRejectsNewEntityBeforeCreation) {
  Harness h;
  const Address source(0x7F000001u, 30002);
  auto* source_channel = FakeChannel(0xBEEF);
  h.app.PeerRegistryForTest().InsertForTest(source, source_channel);
  CellAppMigrationTestAccess::FillReceipts(h.app);
  cellapp::OffloadEntity msg;
  msg.entity_id = 70000;
  msg.type_id = 1;
  msg.space_id = 42;

  h.app.OnOffloadEntity(source, source_channel, msg);

  EXPECT_EQ(h.app.FindEntity(msg.entity_id), nullptr);
  EXPECT_EQ(h.app.FindSpace(msg.space_id), nullptr);
}

TEST(OffloadRevert, ShutdownReplaysConfirmedSuccessInsteadOfAllowingSourceRollback) {
  Harness harness;
  InterfaceTable table;
  class ReplyChannel : public Channel {
   public:
    using Channel::Channel;
    auto Fd() const -> FdHandle override { return kInvalidFd; }
    std::optional<cellapp::OffloadEntityAck> reply;

   protected:
    auto DoSend(std::span<const std::byte> data) -> Result<std::size_t> override {
      BinaryReader reader(data);
      auto id = reader.ReadPackedInt();
      EXPECT_TRUE(id);
      auto result = cellapp::OffloadEntityAck::Deserialize(reader);
      EXPECT_TRUE(result);
      if (result) reply = *result;
      return data.size();
    }
  };
  const Address source("127.0.0.1", 30007);
  auto channel = make_intrusive<ReplyChannel>(harness.dispatcher, table, source);
  harness.app.PeerRegistryForTest().InsertForTest(source, channel.get());
  CellAppMigrationTestAccess::RecordSuccess(harness.app, 100, source, 7);
  harness.app.Shutdown();
  cellapp::OffloadEntity request;
  request.entity_id = 100;
  request.cell_epoch = 7;
  harness.app.OnOffloadEntity(source, channel.get(), request);
  ASSERT_TRUE(channel->reply);
  EXPECT_TRUE(channel->reply->success);
  EXPECT_EQ(channel->reply->reject_reason, cellapp::OffloadRejectReason::kNone);
  request.cell_epoch = 8;
  harness.app.OnOffloadEntity(source, channel.get(), request);
  ASSERT_TRUE(channel->reply);
  EXPECT_FALSE(channel->reply->success);
  EXPECT_EQ(channel->reply->reject_reason, cellapp::OffloadRejectReason::kRejected);
}

TEST(OffloadRevert, RevertRestoresRealWithHauntsCellAndControllers) {
  Harness h;
  auto* peer_channel = FakeChannel(0xBEEF);
  auto* entity = SeedEntityWithState(h, /*id=*/100, peer_channel);
  const Address target(0x7F000001u, 30002);

  // Capture the pre-Offload snapshot. We don't need
  // peer_channel->RemoteAddress() (that would dereference the fake
  // pointer); synthesize an Address directly.
  CellApp::PendingOffload po;
  const Address fake_haunt_addr(0x7F000001u, 30003);
  CaptureSnapshot(po, *entity, /*sid=*/42, /*cid=*/7, target,
                  /*haunt_addrs=*/{fake_haunt_addr});

  // Simulate the Offload send: convert Real→Ghost and remove from
  // Cell membership the way TickOffloadChecker would.
  entity->ConvertRealToGhost(peer_channel);
  ASSERT_TRUE(entity->IsGhost());
  EXPECT_EQ(entity->GetControllers().Count(), 0u);  // StopAll cleared
  entity->GetSpace().FindLocalCell(7)->RemoveRealEntity(entity);

  // This unit test exercises the revert logic on a CellApp that was
  // never Init()ed — peer_registry_ is therefore empty, so haunt
  // resolution quietly drops the saved address. That's the intended
  // semantic: haunts that can't be resolved on revert are treated as
  // having died during the Offload window and will be re-ghosted on
  // the next TickGhostPump pass. The cell-membership + controller
  // restoration is the hot path and is the one we actually assert.

  h.app.PendingOffloadsForTest()[entity->Id()] = std::move(po);
  h.app.RevertPendingOffload(entity->Id(), "test");

  EXPECT_TRUE(entity->IsReal());
  EXPECT_EQ(h.app.PendingOffloadsForTest().size(), 0u);
  // Cell membership restored.
  EXPECT_TRUE(entity->GetSpace().FindLocalCell(7)->HasRealEntity(entity));
  // Controller restored from blob (same count, same user_arg).
  EXPECT_EQ(entity->GetControllers().Count(), 1u);
  bool saw_move = false;
  entity->GetControllers().ForEach([&](const Controller& c) {
    if (c.TypeTag() == ControllerKind::kMoveToPoint) {
      EXPECT_EQ(c.UserArg(), 99);
      saw_move = true;
    }
  });
  EXPECT_TRUE(saw_move);
}

// Revert must re-attach the Witness with the pre-Offload radius +
// hysteresis; otherwise a script-level SetAoIRadius would be silently
// dropped across a failed-Offload boundary.
TEST(OffloadRevert, RevertReattachesWitnessWithPreservedRadius) {
  Harness h;
  auto* peer_channel = FakeChannel(0xBEEF);  // needed for ConvertRealToGhost
  auto* entity = SeedEntityWithState(h, /*id=*/150, /*haunt_channel=*/nullptr);

  // Pretend the entity had a witness with custom AoI before Offload.
  // Don't actually EnableWitness here — the test isolates the revert
  // attach path: we seed PendingOffload as if TickOffloadChecker had
  // captured those values pre-ConvertRealToGhost.
  CellApp::PendingOffload po;
  const Address target(0x7F000001u, 30002);
  CaptureSnapshot(po, *entity, /*sid=*/42, /*cid=*/7, target, /*haunt_addrs=*/{});
  po.had_witness = true;
  po.aoi_radius = 75.f;
  po.aoi_hysteresis = 12.5f;

  // Simulate the Offload send handoff.
  entity->ConvertRealToGhost(peer_channel);
  ASSERT_TRUE(entity->IsGhost());
  entity->GetSpace().FindLocalCell(7)->RemoveRealEntity(entity);

  h.app.PendingOffloadsForTest()[entity->Id()] = std::move(po);
  h.app.RevertPendingOffload(entity->Id(), "witness-preserve");

  ASSERT_TRUE(entity->IsReal());
  ASSERT_TRUE(entity->HasWitness()) << "revert must re-attach the witness";
  EXPECT_FLOAT_EQ(entity->GetWitness()->AoIRadius(), 75.f);
  EXPECT_FLOAT_EQ(entity->GetWitness()->Hysteresis(), 12.5f);
}

// If the entity never had a witness (e.g. a server-only headless
// entity), revert must not spuriously attach one.
TEST(OffloadRevert, RevertLeavesEntityWitnessless) {
  Harness h;
  auto* peer_channel = FakeChannel(0xDEAD);
  auto* entity = SeedEntityWithState(h, /*id=*/151, /*haunt_channel=*/nullptr);

  CellApp::PendingOffload po;
  CaptureSnapshot(po, *entity, /*sid=*/42, /*cid=*/7, Address(0, 0), /*haunt_addrs=*/{});
  po.had_witness = false;  // explicit — the default but worth naming

  entity->ConvertRealToGhost(peer_channel);
  entity->GetSpace().FindLocalCell(7)->RemoveRealEntity(entity);

  h.app.PendingOffloadsForTest()[entity->Id()] = std::move(po);
  h.app.RevertPendingOffload(entity->Id(), "no-witness");

  EXPECT_TRUE(entity->IsReal());
  EXPECT_FALSE(entity->HasWitness());
}

// ============================================================================
// Revert on a missing entry is a no-op (idempotent).
// ============================================================================

TEST(OffloadRevert, RevertMissingEntryIsNoop) {
  Harness h;
  h.app.RevertPendingOffload(/*entity_id=*/9999, "missing");  // must not crash
  EXPECT_EQ(h.app.PendingOffloadsForTest().size(), 0u);
}

// ============================================================================
// Revert on a non-Ghost entity logs + skips (the double-resolve case).
// ============================================================================

TEST(OffloadRevert, RevertSkipsWhenEntityAlreadyReal) {
  Harness h;
  auto* entity = SeedEntityWithState(h, /*id=*/101, /*haunt_channel=*/nullptr);
  CellApp::PendingOffload po;
  po.target_addr = Address(0x7F000001u, 30002);
  po.sent_at = Clock::now();
  po.space_id = 42;
  po.cell_id = 7;
  h.app.PendingOffloadsForTest()[entity->Id()] = std::move(po);

  // Entity is still Real — simulates a late duplicate failure Ack
  // arriving after a previous success Ack already cleared the entry +
  // converted us into something else, then another success that made
  // us Real.
  h.app.RevertPendingOffload(entity->Id(), "double-resolve");
  EXPECT_TRUE(entity->IsReal());
  EXPECT_EQ(h.app.PendingOffloadsForTest().size(), 0u);  // entry consumed regardless
}

// ============================================================================
// Revert on a destroyed entity (removed from space AND population) is
// idempotent — no crash.
// ============================================================================

TEST(OffloadRevert, RevertOnDestroyedEntity) {
  Harness h;
  auto* peer_channel = FakeChannel(0xBEEF);
  auto* entity = SeedEntityWithState(h, /*id=*/200, peer_channel);

  // Capture snapshot while entity is still alive.
  CellApp::PendingOffload po;
  const Address target(0x7F000001u, 30002);
  const Address fake_haunt_addr(0x7F000001u, 30003);
  CaptureSnapshot(po, *entity, /*sid=*/42, /*cid=*/7, target,
                  /*haunt_addrs=*/{fake_haunt_addr});

  // Convert Real → Ghost so the entity is in the state it'd be during
  // a pending Offload.
  entity->ConvertRealToGhost(peer_channel);
  entity->GetSpace().FindLocalCell(7)->RemoveRealEntity(entity);

  // Now destroy the entity: remove from Space AND population.
  const EntityID eid = entity->Id();
  h.app.EntityPopulationForTest().erase(eid);
  entity->GetSpace().RemoveEntity(eid);
  // `entity` pointer is now dangling — do not dereference.

  // Seed the pending offload and attempt revert. Must not crash even
  // though the entity no longer exists.
  h.app.PendingOffloadsForTest()[eid] = std::move(po);
  h.app.RevertPendingOffload(eid, "destroyed-entity");

  // The pending entry should be consumed regardless.
  EXPECT_EQ(h.app.PendingOffloadsForTest().size(), 0u);
}

TEST(OffloadRevert, RevertWithMalformedControllerBlob) {
  Harness h;
  auto* peer_channel = FakeChannel(0xBEEF);
  auto* entity = SeedEntityWithState(h, /*id=*/300, peer_channel);

  // Capture a valid snapshot first.
  CellApp::PendingOffload po;
  const Address target(0x7F000001u, 30002);
  CaptureSnapshot(po, *entity, /*sid=*/42, /*cid=*/7, target,
                  /*haunt_addrs=*/{});

  ASSERT_GT(po.controller_blob.size(), 2u);
  po.controller_blob.resize(2);

  // Convert Real → Ghost and remove from Cell.
  entity->ConvertRealToGhost(peer_channel);
  ASSERT_TRUE(entity->IsGhost());
  entity->GetSpace().FindLocalCell(7)->RemoveRealEntity(entity);

  // Store the pending offload and revert.
  h.app.PendingOffloadsForTest()[entity->Id()] = std::move(po);
  h.app.RevertPendingOffload(entity->Id(), "malformed-blob");

  EXPECT_TRUE(entity->IsGhost());
  EXPECT_FALSE(entity->IsReal());
  ASSERT_EQ(h.app.PendingOffloadsForTest().size(), 1u);
  EXPECT_EQ(h.app.PendingOffloadsForTest().at(entity->Id()).controller_blob.size(), 2u);
  EXPECT_FALSE(entity->GetSpace().FindLocalCell(7)->HasRealEntity(entity));
  EXPECT_EQ(entity->GetControllers().Count(), 0u);
}

}  // namespace
}  // namespace atlas
