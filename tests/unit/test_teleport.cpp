#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "cell.h"
#include "cell_entity.h"
#include "cellapp.h"
#include "cellapp_native_provider.h"
#include "cellappmgr/bsp_tree.h"
#include "foundation/intrusive_ptr.h"
#include "intercell_messages.h"
#include "math/vector3.h"
#include "network/address.h"
#include "network/channel.h"
#include "network/event_dispatcher.h"
#include "network/interface_table.h"
#include "network/network_interface.h"
#include "serialization/binary_stream.h"
#include "space.h"

namespace atlas {

struct CellAppTeleportTestAccess {
  static void LocalRehome(CellApp& app, CellEntity& entity, cellapp::OffloadEntity message) {
    app.LocalTeleportRehome(entity, std::move(message));
  }
  static auto Succeeded(const CellApp& app) -> uint64_t { return app.teleport_succeeded_total_; }
  static void Offload(CellApp& app, CellEntity& entity, Channel* channel, const Address& address,
                      cellapp::OffloadEntity message) {
    app.ProcessOffload(entity, channel, address, std::move(message));
  }
  static void ExhaustRequestIds(CellApp& app) {
    app.next_teleport_request_id_ = std::numeric_limits<uint32_t>::max();
  }
};

namespace {

class RecordingChannel final : public Channel {
 public:
  RecordingChannel(EventDispatcher& d, InterfaceTable& t, const Address& r) : Channel(d, t, r) {}
  [[nodiscard]] auto Fd() const -> FdHandle override { return kInvalidFd; }
  [[nodiscard]] auto DoSend(std::span<const std::byte> data) -> Result<size_t> override {
    sends_.emplace_back(data.begin(), data.end());
    return data.size();
  }
  [[nodiscard]] auto Sends() const -> const std::vector<std::vector<std::byte>>& { return sends_; }
  void ClearSends() { sends_.clear(); }

 private:
  std::vector<std::vector<std::byte>> sends_;
};

// Process-lifetime recording channel; channel holders use IntrusivePtr, so a
// stack object would crash on add_ref/release (mirrors FakeChannel).
auto RecordingChan() -> RecordingChannel* {
  static EventDispatcher d{"teleport-rec"};
  static InterfaceTable t;
  static std::vector<IntrusivePtr<RecordingChannel>> kept;
  kept.push_back(make_intrusive<RecordingChannel>(d, t, Address{}));
  return kept.back().get();
}

struct Harness {
  EventDispatcher dispatcher{"teleport-test"};
  NetworkInterface network{dispatcher};
  CellApp app{dispatcher, network};
  RecordingChannel* mgr_ch{RecordingChan()};

  Harness() { app.SeedCellAppMgrSessionForTest(mgr_ch, /*app_id=*/7, /*pid=*/101); }
};

auto MakeAddr(uint16_t port) -> Address { return Address(0x7F000001u, port); }

// Real entity in `sid` with a local Cell + base_addr so it is offloadable.
auto SeedReal(Harness& h, EntityID id, SpaceID sid) -> CellEntity* {
  auto& spaces = h.app.Spaces();
  auto* space = spaces.emplace(sid, std::make_unique<Space>(sid)).first->second.get();
  auto* cell = space->AddLocalCell(std::make_unique<Cell>(*space, /*cell_id=*/7, CellBounds{}));
  auto* entity = space->AddEntity(std::make_unique<CellEntity>(
      id, /*type=*/uint16_t{1}, *space, math::Vector3{0, 0, 0}, math::Vector3{1, 0, 0}));
  entity->SetBaseAddr(MakeAddr(20000));
  cell->AddRealEntity(entity);
  h.app.EntityPopulationForTest()[id] = entity;
  return entity;
}

auto CountRequests(const RecordingChannel& ch) -> std::size_t {
  std::size_t n = 0;
  for (const auto& frame : ch.Sends()) {
    BinaryReader r(std::span<const std::byte>(frame.data(), frame.size()));
    const auto id = r.ReadPackedInt();
    if (id && *id == cellappmgr::ResolveSpaceHostRequest::Descriptor().id) ++n;
  }
  return n;
}

auto FirstOffload(const RecordingChannel& ch) -> cellapp::OffloadEntity {
  for (const auto& frame : ch.Sends()) {
    BinaryReader r(std::span<const std::byte>(frame.data(), frame.size()));
    const auto id = r.ReadPackedInt();
    if (!id || *id != cellapp::OffloadEntity::Descriptor().id) continue;
    const auto len = r.ReadPackedInt();
    if (!len) continue;
    const auto payload = r.ReadBytes(*len);
    if (!payload) continue;
    BinaryReader mr(*payload);
    auto msg = cellapp::OffloadEntity::Deserialize(mr);
    if (msg.HasValue()) return *msg;
  }
  ADD_FAILURE() << "No OffloadEntity in sends";
  return {};
}

auto FirstAck(const RecordingChannel& ch) -> cellapp::OffloadEntityAck {
  for (const auto& frame : ch.Sends()) {
    BinaryReader r(std::span<const std::byte>(frame.data(), frame.size()));
    const auto id = r.ReadPackedInt();
    if (!id || *id != cellapp::OffloadEntityAck::Descriptor().id) continue;
    auto msg = cellapp::OffloadEntityAck::Deserialize(r);
    if (msg.HasValue()) return *msg;
  }
  ADD_FAILURE() << "No OffloadEntityAck in sends";
  return {};
}

TEST(Teleport, RequestRejectsUnknownEntity) {
  Harness h;
  EXPECT_FALSE(h.app.RequestTeleport(/*entity_id=*/999, /*target=*/2, {1, 0, 1}, {1, 0, 0}));
  EXPECT_EQ(h.app.PendingTeleportsForTest().size(), 0u);
}

TEST(Teleport, RequestFailsWithoutMgrChannel) {
  Harness h;
  h.app.SeedCellAppMgrSessionForTest(nullptr, 7, 101);  // drop the mgr channel
  SeedReal(h, /*id=*/100, /*sid=*/1);
  EXPECT_FALSE(h.app.RequestTeleport(100, 2, {1, 0, 1}, {1, 0, 0}));
}

TEST(Teleport, RequestSendsResolveAndTracksPending) {
  Harness h;
  SeedReal(h, /*id=*/100, /*sid=*/1);
  EXPECT_TRUE(h.app.RequestTeleport(100, /*target_space=*/2, {5, 0, 6}, {1, 0, 0}));
  EXPECT_EQ(CountRequests(*h.mgr_ch), 1u);
  EXPECT_EQ(h.app.PendingTeleportsForTest().size(), 1u);
}

TEST(Teleport, ResolveNotFoundAbortsAndKeepsReal) {
  Harness h;
  auto* entity = SeedReal(h, /*id=*/100, /*sid=*/1);
  ASSERT_TRUE(h.app.RequestTeleport(100, 2, {5, 0, 6}, {1, 0, 0}));
  const uint32_t rid = h.app.PendingTeleportsForTest().begin()->first;

  cellappmgr::ResolveSpaceHostReply reply;
  reply.request_id = rid;
  reply.entity_id = 100;
  reply.space_id = 2;
  reply.found = false;
  h.app.OnResolveSpaceHostReply(MakeAddr(40000), h.mgr_ch, reply);

  EXPECT_TRUE(entity->IsReal());
  EXPECT_EQ(h.app.PendingTeleportsForTest().size(), 0u);
}

TEST(Teleport, ResolveUnknownRequestIsNoop) {
  Harness h;
  cellappmgr::ResolveSpaceHostReply reply;
  reply.request_id = 12345;  // never tracked
  reply.found = true;
  reply.host_addr = MakeAddr(40000);
  h.app.OnResolveSpaceHostReply(MakeAddr(40000), h.mgr_ch, reply);  // must not crash
  EXPECT_EQ(h.app.PendingTeleportsForTest().size(), 0u);
}

TEST(Teleport, ConcurrentRequestForSameEntityIsRejected) {
  Harness h;
  SeedReal(h, 100, 1);
  ASSERT_TRUE(h.app.RequestTeleport(100, 2, {}, {}));
  EXPECT_FALSE(h.app.RequestTeleport(100, 3, {}, {}));
  EXPECT_EQ(h.app.PendingTeleportsForTest().size(), 1u);
  EXPECT_EQ(CountRequests(*h.mgr_ch), 1u);
}

TEST(Teleport, ExhaustedRequestIdsCannotAliasEarlierReplies) {
  Harness h;
  SeedReal(h, 100, 1);
  CellAppTeleportTestAccess::ExhaustRequestIds(h.app);
  ASSERT_TRUE(h.app.RequestTeleport(100, 2, {}, {}));
  ASSERT_TRUE(h.app.PendingTeleportsForTest().contains(std::numeric_limits<uint32_t>::max()));
  h.app.PendingTeleportsForTest().clear();
  EXPECT_FALSE(h.app.RequestTeleport(100, 2, {}, {}));
  EXPECT_TRUE(h.app.PendingTeleportsForTest().empty());
  EXPECT_EQ(CountRequests(*h.mgr_ch), 1u);
}

TEST(Teleport, ResolveReplyMustMatchEntityAndSpaceBeforeConsumingRequest) {
  Harness h;
  auto* entity = SeedReal(h, 100, 1);
  ASSERT_TRUE(h.app.RequestTeleport(100, 2, {}, {}));
  const auto rid = h.app.PendingTeleportsForTest().begin()->first;
  cellappmgr::ResolveSpaceHostReply reply;
  reply.request_id = rid;
  reply.entity_id = 101;
  reply.space_id = 2;
  h.app.OnResolveSpaceHostReply({}, h.mgr_ch, reply);
  EXPECT_EQ(h.app.PendingTeleportsForTest().size(), 1u);
  reply.entity_id = 100;
  reply.space_id = 3;
  h.app.OnResolveSpaceHostReply({}, h.mgr_ch, reply);
  EXPECT_EQ(h.app.PendingTeleportsForTest().size(), 1u);
  reply.space_id = 2;
  h.app.OnResolveSpaceHostReply({}, h.mgr_ch, reply);
  EXPECT_TRUE(h.app.PendingTeleportsForTest().empty());
  EXPECT_TRUE(entity->IsReal());
}

TEST(Teleport, ResolveCannotMigrateANewerEntityEpoch) {
  Harness h;
  auto* entity = SeedReal(h, 100, 1);
  const auto target = MakeAddr(40000);
  auto* peer = RecordingChan();
  h.app.PeerRegistryForTest().InsertForTest(target, peer);
  ASSERT_TRUE(h.app.RequestTeleport(100, 2, {}, {}));
  entity->SetCellEpoch(entity->CellEpoch() + 1);
  cellappmgr::ResolveSpaceHostReply reply;
  reply.request_id = h.app.PendingTeleportsForTest().begin()->first;
  reply.entity_id = 100;
  reply.space_id = 2;
  reply.found = true;
  reply.host_addr = target;
  h.app.OnResolveSpaceHostReply({}, h.mgr_ch, reply);
  EXPECT_TRUE(entity->IsReal());
  EXPECT_TRUE(h.app.PendingTeleportsForTest().empty());
  EXPECT_TRUE(h.app.PendingOffloadsForTest().empty());
  EXPECT_TRUE(peer->Sends().empty());
}

TEST(Teleport, ExhaustedEpochRejectsBeforeRequestOrPoseChange) {
  Harness h;
  auto* entity = SeedReal(h, 100, 1);
  const auto before = entity->Position();
  entity->SetCellEpoch(std::numeric_limits<uint32_t>::max());
  EXPECT_FALSE(h.app.RequestTeleport(100, 2, {7, 0, 8}, {}));
  EXPECT_TRUE(h.app.PendingTeleportsForTest().empty());
  EXPECT_EQ(CountRequests(*h.mgr_ch), 0u);
  EXPECT_EQ(entity->Position(), before);
}

TEST(Teleport, ResolveFoundOffloadsToTargetAsTeleport) {
  Harness h;
  auto* entity = SeedReal(h, /*id=*/100, /*sid=*/1);
  const Address target = MakeAddr(40000);
  auto* peer_ch = RecordingChan();
  h.app.PeerRegistryForTest().InsertForTest(target, peer_ch);

  ASSERT_TRUE(h.app.RequestTeleport(100, /*target_space=*/2, {7, 0, 8}, {1, 0, 0}));
  const uint32_t rid = h.app.PendingTeleportsForTest().begin()->first;

  cellappmgr::ResolveSpaceHostReply reply;
  reply.request_id = rid;
  reply.entity_id = 100;
  reply.space_id = 2;
  reply.found = true;
  reply.host_addr = target;
  reply.cell_id = 3;
  reply.geometry_version = 99;  // ignored; teleport ships geometry_version=0
  h.app.OnResolveSpaceHostReply(target, h.mgr_ch, reply);

  EXPECT_TRUE(entity->IsGhost());
  EXPECT_EQ(h.app.PendingTeleportsForTest().size(), 0u);
  EXPECT_EQ(h.app.PendingOffloadsForTest().count(100), 1u);

  auto off = FirstOffload(*peer_ch);
  EXPECT_EQ(off.entity_id, 100u);
  EXPECT_TRUE(off.is_teleport);
  EXPECT_EQ(off.space_id, 2u);
  EXPECT_EQ(off.geometry_version, 0u);
  EXPECT_FLOAT_EQ(off.position.x, 7.f);
  EXPECT_FLOAT_EQ(off.position.z, 8.f);
  EXPECT_EQ(entity->Position(), (math::Vector3{0, 0, 0}));
  cellapp::OffloadEntityAck rejection;
  rejection.entity_id = 100;
  rejection.cell_epoch = off.cell_epoch;
  rejection.reject_reason = cellapp::OffloadRejectReason::kTargetMissing;
  h.app.OnOffloadEntityAck(target, peer_ch, rejection);
  EXPECT_TRUE(entity->IsReal());
  EXPECT_EQ(entity->GetSpace().Id(), 1u);
  EXPECT_EQ(entity->Position(), (math::Vector3{0, 0, 0}));
}

TEST(Teleport, DuplicateOffloadReplaysSuccessWithoutResurrectingDestroyedEntity) {
  Harness h;
  const auto source = MakeAddr(50000);
  auto* channel = RecordingChan();
  h.app.PeerRegistryForTest().InsertForTest(source, channel);
  cellapp::OffloadEntity msg;
  msg.entity_id = 100;
  msg.type_id = 1;
  msg.space_id = 42;
  msg.cell_epoch = 7;
  h.app.OnOffloadEntity(source, channel, msg);
  ASSERT_NE(h.app.FindEntity(100), nullptr);
  EXPECT_EQ(h.app.FindEntity(100)->CellEpoch(), 8u);
  EXPECT_TRUE(FirstAck(*channel).success);
  channel->ClearSends();

  h.app.OnOffloadEntity(source, channel, msg);
  EXPECT_TRUE(FirstAck(*channel).success);
  EXPECT_EQ(FirstAck(*channel).cell_epoch, 7u);
  channel->ClearSends();
  h.app.EntityPopulationForTest().erase(100);
  h.app.FindSpace(42)->RemoveEntity(100);

  h.app.OnOffloadEntity(source, channel, msg);

  EXPECT_EQ(h.app.FindEntity(100), nullptr);
  EXPECT_TRUE(FirstAck(*channel).success);
}

TEST(Teleport, InvalidControllerStateRejectsWithoutInstallingAReal) {
  Harness h;
  const auto source = MakeAddr(50000);
  auto* channel = RecordingChan();
  h.app.PeerRegistryForTest().InsertForTest(source, channel);
  cellapp::OffloadEntity msg;
  msg.entity_id = 100;
  msg.type_id = 1;
  msg.space_id = 42;
  msg.controller_data = {std::byte{1}};

  h.app.OnOffloadEntity(source, channel, msg);

  EXPECT_EQ(h.app.FindEntity(100), nullptr);
  const auto ack = FirstAck(*channel);
  EXPECT_FALSE(ack.success);
  EXPECT_EQ(ack.reject_reason, cellapp::OffloadRejectReason::kRestoreFailed);
}

TEST(Teleport, OnOffloadTeleportToUnhostedSpaceRejects) {
  Harness h;
  const Address src = MakeAddr(50000);
  auto* src_ch = RecordingChan();
  h.app.PeerRegistryForTest().InsertForTest(src, src_ch);  // trust the sender

  cellapp::OffloadEntity msg;
  msg.entity_id = 100;
  msg.type_id = 1;
  msg.space_id = 777;  // not hosted on this CellApp
  msg.is_teleport = true;
  msg.geometry_version = 0;
  msg.base_addr = MakeAddr(20000);
  h.app.OnOffloadEntity(src, src_ch, msg);

  auto ack = FirstAck(*src_ch);
  EXPECT_EQ(ack.entity_id, 100u);
  EXPECT_FALSE(ack.success);
  EXPECT_EQ(ack.reject_reason, cellapp::OffloadRejectReason::kTargetMissing);
}

TEST(Teleport, OnOffloadTeleportToPositionOutsideLocalCellRejects) {
  Harness h;
  const Address src = MakeAddr(50000);
  auto* src_ch = RecordingChan();
  h.app.PeerRegistryForTest().InsertForTest(src, src_ch);

  // Space is hosted (local cell id=7) but the BSP maps the arrival pos to leaf
  // id=1, which isn't local here - the leaf moved away between resolve and arrival.
  auto& spaces = h.app.Spaces();
  auto* space = spaces.emplace(2, std::make_unique<Space>(2)).first->second.get();
  space->AddLocalCell(std::make_unique<Cell>(*space, /*cell_id=*/7, CellBounds{}));
  BSPTree tree;
  tree.InitSingleCell(CellInfo{/*cell_id=*/1, MakeAddr(60000), /*bounds=*/{}, 0.f, 0});
  space->SetBspTree(std::move(tree));

  cellapp::OffloadEntity msg;
  msg.entity_id = 101;
  msg.type_id = 1;
  msg.space_id = 2;
  msg.is_teleport = true;
  msg.geometry_version = 0;
  msg.position = {5.f, 0.f, 5.f};
  msg.base_addr = MakeAddr(20000);
  h.app.OnOffloadEntity(src, src_ch, msg);

  auto ack = FirstAck(*src_ch);
  EXPECT_EQ(ack.entity_id, 101u);
  EXPECT_FALSE(ack.success);
  EXPECT_EQ(ack.reject_reason, cellapp::OffloadRejectReason::kStaleGeometry);
  EXPECT_EQ(h.app.EntityPopulationForTest().count(101), 0u);  // not stranded
}

int g_teleport_failed_calls = 0;
uint32_t g_teleport_failed_eid = 0;
uint8_t g_teleport_failed_reason = 0xFF;

TEST(Teleport, ResolveNotFoundFiresTeleportFailedCallback) {
  Harness h;
  auto provider = h.app.CreateNativeProviderForTest();  // sets native_provider_
  g_teleport_failed_calls = 0;
  g_teleport_failed_eid = 0;
  g_teleport_failed_reason = 0xFF;
  h.app.NativeProvider()->SetTeleportFailedFnForTest([](uint32_t eid, uint8_t reason) {
    ++g_teleport_failed_calls;
    g_teleport_failed_eid = eid;
    g_teleport_failed_reason = reason;
  });

  SeedReal(h, /*id=*/100, /*sid=*/1);
  ASSERT_TRUE(h.app.RequestTeleport(100, /*target_space=*/2, {5, 0, 6}, {1, 0, 0}));
  const uint32_t rid = h.app.PendingTeleportsForTest().begin()->first;

  cellappmgr::ResolveSpaceHostReply reply;
  reply.request_id = rid;
  reply.entity_id = 100;
  reply.space_id = 2;
  reply.found = false;
  h.app.OnResolveSpaceHostReply(MakeAddr(40000), h.mgr_ch, reply);

  EXPECT_EQ(g_teleport_failed_calls, 1);
  EXPECT_EQ(g_teleport_failed_eid, 100u);
  EXPECT_EQ(g_teleport_failed_reason, static_cast<uint8_t>(TeleportFailReason::kTargetUnhosted));
}

TEST(Teleport, RequestRejectedWhenPendingAtCap) {
  Harness h;
  SeedReal(h, /*id=*/100, /*sid=*/1);
  auto& pending = h.app.PendingTeleportsForTest();
  for (uint32_t i = 1; i <= 1024; ++i) pending[i] = CellApp::PendingTeleport{};  // kMaxPendingTeleports
  EXPECT_FALSE(h.app.RequestTeleport(100, /*target_space=*/2, {1, 0, 1}, {1, 0, 0}));
}

TEST(Teleport, LocalRestoreFailureKeepsSourceAndDoesNotReportSuccess) {
  Harness h;
  auto* source = SeedReal(h, 100, 1);
  auto& spaces = h.app.Spaces();
  auto* destination = spaces.emplace(2, std::make_unique<Space>(2)).first->second.get();
  destination->AddLocalCell(std::make_unique<Cell>(*destination, 1, CellBounds{}));
  BSPTree tree;
  tree.InitSingleCell(CellInfo{1, h.app.ResolveSelfAddrForTest(), {}, 0.f, 0});
  destination->SetBspTree(std::move(tree));
  cellapp::OffloadEntity message;
  message.entity_id = 100;
  message.type_id = 1;
  message.space_id = 2;
  message.is_teleport = true;
  message.controller_data = {std::byte{1}};
  CellAppTeleportTestAccess::LocalRehome(h.app, *source, message);
  EXPECT_EQ(CellAppTeleportTestAccess::Succeeded(h.app), 0u);
  auto* restored = h.app.FindEntity(100);
  ASSERT_NE(restored, nullptr);
  EXPECT_TRUE(restored->IsReal());
  EXPECT_EQ(restored->GetSpace().Id(), 1u);
  EXPECT_TRUE(spaces.at(1)->FindLocalCell(7)->HasRealEntity(restored));
  EXPECT_EQ(destination->FindEntity(100), nullptr);
  EXPECT_TRUE(h.app.PendingOffloadsForTest().empty());
}

TEST(Teleport, FailedSendRetainsPassiveGhostAndRecoverySnapshot) {
  Harness h;
  auto* source = SeedReal(h, 100, 1);
  auto* channel = RecordingChan();
  channel->Condemn();
  cellapp::OffloadEntity message;
  message.entity_id = 100;
  message.type_id = 1;
  message.space_id = 1;
  message.persistent_blob = {std::byte{7}};
  CellAppTeleportTestAccess::Offload(h.app, *source, channel, MakeAddr(40000), message);
  EXPECT_TRUE(source->IsGhost());
  EXPECT_FALSE(h.app.FindSpace(1)->FindLocalCell(7)->HasRealEntity(source));
  const auto& pending = h.app.PendingOffloadsForTest();
  ASSERT_EQ(pending.size(), 1u);
  EXPECT_TRUE(pending.at(100).outcome_unknown);
  EXPECT_EQ(pending.at(100).persistent_blob, message.persistent_blob);
}

TEST(Teleport, LocalRollbackFailureKeepsPassiveSourceAndSnapshot) {
  Harness h;
  auto* source = SeedReal(h, 100, 1);
  auto* destination = h.app.Spaces().emplace(2, std::make_unique<Space>(2)).first->second.get();
  destination->AddLocalCell(std::make_unique<Cell>(*destination, 1, CellBounds{}));
  BSPTree tree;
  tree.InitSingleCell(CellInfo{1, h.app.ResolveSelfAddrForTest(), {}, 0.f, 0});
  destination->SetBspTree(std::move(tree));
  auto provider = h.app.CreateNativeProviderForTest();
  static CellApp* app;
  app = &h.app;
  auto restore = +[](uint32_t id, uint16_t, int64_t, const uint8_t*, int32_t) {
    const auto pending = app->PendingOffloadsForTest().find(id);
    if (pending != app->PendingOffloadsForTest().end())
      pending->second.controller_blob = {std::byte{1}};
  };
  std::array<void*, 4> callbacks{reinterpret_cast<void*>(restore), nullptr, nullptr, nullptr};
  h.app.NativeProvider()->SetNativeCallbacks(callbacks.data(), sizeof(callbacks));
  cellapp::OffloadEntity message;
  message.entity_id = 100;
  message.type_id = 1;
  message.space_id = 2;
  message.is_teleport = true;
  message.persistent_blob = {std::byte{7}};
  message.controller_data = {std::byte{1}};
  CellAppTeleportTestAccess::LocalRehome(h.app, *source, message);
  app = nullptr;
  EXPECT_EQ(h.app.FindEntity(100), source);
  EXPECT_TRUE(source->IsGhost());
  EXPECT_EQ(source->GetSpace().Id(), 1u);
  EXPECT_FALSE(h.app.FindSpace(1)->FindLocalCell(7)->HasRealEntity(source));
  const auto& pending = h.app.PendingOffloadsForTest();
  ASSERT_EQ(pending.size(), 1u);
  EXPECT_TRUE(pending.at(100).outcome_unknown);
  EXPECT_EQ(pending.at(100).persistent_blob, message.persistent_blob);
  EXPECT_EQ(CellAppTeleportTestAccess::Succeeded(h.app), 0u);
}

TEST(Teleport, SameSpaceRehomeLeavesOneLiveEntity) {
  Harness h;
  auto* source = SeedReal(h, 100, 1);
  auto* space = h.app.FindSpace(1);
  BSPTree tree;
  tree.InitSingleCell(CellInfo{7, h.app.ResolveSelfAddrForTest(), {}, 0.f, 0});
  space->SetBspTree(std::move(tree));
  cellapp::OffloadEntity message;
  message.entity_id = 100;
  message.type_id = 1;
  message.space_id = 1;
  message.is_teleport = true;
  message.position = {3, 0, 4};
  CellAppTeleportTestAccess::LocalRehome(h.app, *source, message);
  auto* moved = h.app.FindEntity(100);
  ASSERT_NE(moved, nullptr);
  EXPECT_TRUE(moved->IsReal());
  EXPECT_EQ(moved->Position(), message.position);
  EXPECT_EQ(space->EntityCount(), 1u);
  EXPECT_TRUE(space->FindLocalCell(7)->HasRealEntity(moved));
  EXPECT_TRUE(h.app.PendingOffloadsForTest().empty());
  EXPECT_EQ(CellAppTeleportTestAccess::Succeeded(h.app), 1u);
}

TEST(Teleport, FailedSendCanOnlyRollbackAfterExplicitMatchingRejection) {
  Harness h;
  auto* source = SeedReal(h, 100, 1);
  source->SetPositionAndDirection({3, 0, 4}, {1, 0, 0});
  auto* channel = RecordingChan();
  const auto target = MakeAddr(40000);
  h.app.PeerRegistryForTest().InsertForTest(target, channel);
  channel->Condemn();
  cellapp::OffloadEntity message;
  message.entity_id = 100;
  message.type_id = 1;
  message.space_id = 2;
  message.is_teleport = true;
  message.position = {30, 0, 40};
  CellAppTeleportTestAccess::Offload(h.app, *source, channel, target, message);
  cellapp::OffloadEntityAck rejected;
  rejected.entity_id = 100;
  rejected.cell_epoch = message.cell_epoch;
  rejected.success = false;
  rejected.reject_reason = cellapp::OffloadRejectReason::kTargetMissing;
  h.app.OnOffloadEntityAck(target, channel, rejected);
  EXPECT_TRUE(source->IsReal());
  EXPECT_EQ(source->Position(), (math::Vector3{3, 0, 4}));
  EXPECT_TRUE(h.app.PendingOffloadsForTest().empty());
  EXPECT_TRUE(h.app.FindSpace(1)->FindLocalCell(7)->HasRealEntity(source));
}

TEST(Teleport, SameCellAppRehomesAcrossSpaces) {
  Harness h;
  auto* src = SeedReal(h, /*id=*/100, /*sid=*/1);
  src->SetBaseAddr(Address{});  // NPC: skip the base CurrentCell network send
  ASSERT_EQ(src->GetSpace().Id(), 1u);

  const Address self = h.app.ResolveSelfAddrForTest();

  // Destination space 2 hosted on THIS cellapp.
  auto& spaces = h.app.Spaces();
  auto* dst = spaces.emplace(2, std::make_unique<Space>(2)).first->second.get();
  dst->AddLocalCell(std::make_unique<Cell>(*dst, /*cell_id=*/1, CellBounds{}));
  BSPTree tree;
  tree.InitSingleCell(CellInfo{/*cell_id=*/1, self, /*bounds=*/{}, 0.f, 0});
  dst->SetBspTree(std::move(tree));

  const math::Vector3 pos{12.f, 0.f, 9.f};
  h.app.PendingTeleportsForTest()[1] =
      CellApp::PendingTeleport{100, 2, pos, {1.f, 0.f, 0.f}, Clock::now()};
  cellappmgr::ResolveSpaceHostReply reply;
  reply.request_id = 1;
  reply.entity_id = 100;
  reply.space_id = 2;
  reply.found = true;
  reply.host_addr = self;  // same-cellapp path
  h.app.OnResolveSpaceHostReply(self, h.mgr_ch, reply);

  auto* moved = h.app.FindEntity(100);  // src is dangling after the re-home
  ASSERT_NE(moved, nullptr);
  EXPECT_TRUE(moved->IsReal());
  EXPECT_EQ(moved->GetSpace().Id(), 2u);
  EXPECT_FLOAT_EQ(moved->Position().x, pos.x);
  EXPECT_FLOAT_EQ(moved->Position().z, pos.z);
  EXPECT_TRUE(dst->FindLocalCell(1)->HasRealEntity(moved));
  EXPECT_EQ(spaces.at(1)->FindEntity(100), nullptr);  // gone from the source space
}

}  // namespace
}  // namespace atlas
