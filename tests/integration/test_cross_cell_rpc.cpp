#include <chrono>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "baseapp/baseapp_messages.h"
#include "cell.h"
#include "cell_entity.h"
#include "cellapp.h"
#include "cellapp_messages.h"
#include "cellapp_native_provider.h"
#include "clrscript/native_api_provider.h"
#include "coro/entity_rpc_reply.h"
#include "math/vector3.h"
#include "network/channel.h"
#include "network/event_dispatcher.h"
#include "network/interface_table.h"
#include "network/network_interface.h"
#include "network/reliable_udp.h"
#include "space.h"

namespace atlas {
namespace {

struct DispatchRecord {
  uint32_t entity_id;
  uint32_t rpc_id;
  int32_t payload_len;
  uint8_t first_byte;
};
std::vector<DispatchRecord>* g_dispatched = nullptr;

extern "C" void FakeDispatchRpc(uint32_t eid, uint32_t rid, intptr_t,
                                const uint8_t* payload, int32_t len, uint64_t) {
  if (g_dispatched) {
    g_dispatched->push_back({eid, rid, len, len > 0 ? payload[0] : uint8_t{0}});
  }
}

#pragma pack(push, 1)
struct CallbackTable {
  void* restore_entity;
  void* get_entity_data;
  void* entity_destroyed;
  void* dispatch_rpc;
  void* get_owner_snapshot;
  void* serialize_entity;
  void* proximity_event;
  void* coro_on_rpc_complete;
  void* entity_lifecycle_cancel;
  void* timer_event;
  void* entity_migrating_out;
  void* restore_ghost;
  void* destroy_ghost;
};
#pragma pack(pop)

struct Host {
  EventDispatcher dispatcher;
  NetworkInterface network;
  CellApp app;
  std::unique_ptr<INativeApiProvider> provider_holder;

  explicit Host(std::string name)
      : dispatcher(std::move(name)), network(dispatcher), app(dispatcher, network) {
    dispatcher.SetMaxPollWait(Milliseconds(1));
    auto& t = network.InterfaceTable();
    (void)t.RegisterTypedHandler<cellapp::InternalCellRpc>(
        [this](const Address& src, Channel* ch, const cellapp::InternalCellRpc& m) {
          app.OnInternalCellRpc(src, ch, m);
        });
  }

  auto StartServer() -> Address {
    auto r = network.StartRudpServer(Address("127.0.0.1", 0));
    EXPECT_TRUE(r.HasValue());
    return network.RudpAddress();
  }

  void WireProviderWithDispatch() {
    provider_holder = app.CreateNativeProviderForTest();
    CallbackTable table{};
    table.dispatch_rpc = reinterpret_cast<void*>(&FakeDispatchRpc);
    app.NativeProvider()->SetNativeCallbacks(&table, sizeof(table));
    network.InterfaceTable().SetPreDispatchHook(
        [this](Channel* channel, MessageID id, std::span<const std::byte> payload) {
          return app.GetRpcRegistry().TryDispatch(id, payload, channel);
        });
  }
};

auto PumpUntil(Host& a, Host& b, const std::function<bool()>& pred,
               std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) -> bool {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    a.network.FlushDirtySendChannels();
    b.network.FlushDirtySendChannels();
    a.dispatcher.ProcessOnce();
    b.dispatcher.ProcessOnce();
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return false;
}

TEST(CrossCellRpc, GhostSendCellRpcReachesRealsDispatcher) {
  std::vector<DispatchRecord> dispatched;
  g_dispatched = &dispatched;

  Host shooter("shooter");
  Host target("target");
  shooter.StartServer();
  auto target_addr = target.StartServer();

  shooter.WireProviderWithDispatch();
  target.WireProviderWithDispatch();

  cellapp::CreateCellEntity create_msg;
  create_msg.entity_id = 7000;
  create_msg.type_id = 1;
  create_msg.space_id = 1;
  create_msg.position = {5, 0, 5};
  create_msg.direction = {1, 0, 0};
  target.app.OnCreateCellEntity({}, nullptr, create_msg);
  ASSERT_NE(target.app.FindRealEntity(7000), nullptr);

  auto shooter_to_target = shooter.network.ConnectRudp(target_addr);
  ASSERT_TRUE(shooter_to_target.HasValue());
  shooter.app.PeerRegistryForTest().InsertForTest(target_addr, *shooter_to_target);

  cellapp::CreateSpace cs{1};
  shooter.app.OnCreateSpace({}, nullptr, cs);
  auto* shooter_space = shooter.app.FindSpace(1);
  ASSERT_NE(shooter_space, nullptr);

  auto ghost = std::make_unique<CellEntity>(
      CellEntity::GhostTag{}, /*id=*/7000, /*type_id=*/1, *shooter_space,
      math::Vector3{5, 0, 5}, math::Vector3{1, 0, 0},
      /*real_channel=*/static_cast<Channel*>(*shooter_to_target));
  auto* ghost_ptr = shooter_space->AddEntity(std::move(ghost));
  shooter.app.EntityPopulationForTest()[7000] = ghost_ptr;
  ASSERT_TRUE(ghost_ptr->IsGhost());

  // Payload that Npc.TakeDamage(int amount, uint attackerId) would deserialise
  // — Avatar_TakeDamage = 0x800204 in the codegen; this test uses a fixed rpc
  // id that the fake dispatcher captures verbatim (no real method dispatch).
  std::array<uint8_t, 8> payload{};
  const int32_t amount = 25;
  const uint32_t attacker = 42;
  std::memcpy(payload.data(), &amount, sizeof(amount));
  std::memcpy(payload.data() + 4, &attacker, sizeof(attacker));

  shooter.app.NativeProvider()->SendCellRpc(
      7000, 0x800401u, reinterpret_cast<const std::byte*>(payload.data()),
      static_cast<int32_t>(payload.size()), /*trace_id=*/0);

  const bool dispatched_ok =
      PumpUntil(shooter, target, [&]() { return !dispatched.empty(); });
  ASSERT_TRUE(dispatched_ok) << "InternalCellRpc did not reach target dispatcher";
  ASSERT_EQ(dispatched.size(), 1u);
  EXPECT_EQ(dispatched[0].entity_id, 7000u);
  EXPECT_EQ(dispatched[0].rpc_id, 0x800401u);
  EXPECT_EQ(dispatched[0].payload_len, 8);
  EXPECT_EQ(dispatched[0].first_byte, static_cast<uint8_t>(amount));

  g_dispatched = nullptr;
}

TEST(CrossCellRpc, CellToBaseReplyUsesBoundChannelAndFullRequestId) {
  Host caller("cell_caller");
  Host receiver("base_receiver");
  caller.StartServer();
  auto receiver_addr = receiver.StartServer();
  caller.WireProviderWithDispatch();
  cellapp::CreateCellEntity create;
  create.entity_id = 7100;
  create.type_id = 1;
  create.space_id = 1;
  caller.app.OnCreateCellEntity({}, nullptr, create);
  auto* entity = caller.app.FindRealEntity(7100);
  ASSERT_NE(entity, nullptr);
  entity->SetBaseAddr(receiver_addr);
  constexpr uint64_t kRequestId = (uint64_t{1} << 32) + 9;
  bool received = false;
  ASSERT_TRUE(receiver.network.InterfaceTable().RegisterTypedHandler<baseapp::CellRpcForward>(
      [&](const Address&, Channel* channel, const baseapp::CellRpcForward& message) {
        received = true;
        EXPECT_EQ(message.entity_id, 7100u);
        EXPECT_EQ(message.rpc_id, 0x80C00001u);
        BinaryReader reader(message.payload);
        auto request_id = reader.Read<uint64_t>();
        ASSERT_TRUE(request_id);
        EXPECT_EQ(*request_id, kRequestId);
        EXPECT_TRUE(entity_rpc_reply::SendSuccess(*channel, *request_id, {}));
      }));
  int replies = 0;
  caller.app.GetRpcRegistry().RegisterPending(
      EntityRpcReply::Descriptor().id, kRequestId, [&](std::span<const std::byte>) { ++replies; },
      [](Error) { ADD_FAILURE(); }, Milliseconds(5000));
  BinaryWriter request;
  request.Write(kRequestId);
  caller.app.NativeProvider()->SendBaseRpc(7100, 0x80C00001, request.Data().data(),
                                           static_cast<int32_t>(request.Data().size()), 0);
  EXPECT_TRUE(PumpUntil(caller, receiver, [&] { return replies == 1; }));
  EXPECT_TRUE(received);
  EXPECT_EQ(caller.app.GetRpcRegistry().PendingCount(), 0u);
}

TEST(CrossCellRpc, MissingRemoteCellReturnsReceiverGoneWithoutWaitingForTimeout) {
  Host caller("missing_caller");
  Host receiver("missing_receiver");
  caller.StartServer();
  auto receiver_addr = receiver.StartServer();
  caller.WireProviderWithDispatch();
  receiver.WireProviderWithDispatch();
  auto channel = caller.network.ConnectRudp(receiver_addr);
  ASSERT_TRUE(channel);
  int error_code = 0;
  caller.app.GetRpcRegistry().RegisterPending(
      EntityRpcReply::Descriptor().id, 77,
      [&](std::span<const std::byte> payload) {
        BinaryReader reader(payload);
        ASSERT_TRUE(reader.Read<uint64_t>());
        auto error = reader.Read<int32_t>();
        ASSERT_TRUE(error);
        error_code = *error;
      },
      [](Error) { ADD_FAILURE(); }, Milliseconds(5000), *channel);
  cellapp::InternalCellRpc message;
  message.target_entity_id = 9999;
  message.rpc_id = 0x80800001;
  BinaryWriter request;
  request.Write(uint64_t{77});
  message.payload.assign(request.Data().begin(), request.Data().end());
  ASSERT_TRUE((*channel)->SendMessage(message));
  EXPECT_TRUE(PumpUntil(caller, receiver, [&] { return error_code != 0; }));
  EXPECT_EQ(error_code, -4);
  EXPECT_EQ(caller.app.GetRpcRegistry().PendingCount(), 0u);
  EXPECT_EQ(receiver.app.GetRpcRegistry().ReplyCount(), 0u);
}

}  // namespace
}  // namespace atlas
