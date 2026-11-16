#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "baseapp/baseapp.h"
#include "baseapp/baseapp_messages.h"
#include "baseapp/baseapp_native_provider.h"
#include "cellapp/cellapp_messages.h"
#include "coro/entity_rpc_reply.h"
#include "entitydef/entity_def_registry.h"
#include "entitydef/entity_type_descriptor.h"
#include "foundation/intrusive_ptr.h"
#include "network/channel.h"
#include "network/event_dispatcher.h"
#include "network/interface_table.h"
#include "network/network_interface.h"
#include "serialization/binary_stream.h"

namespace atlas {
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

 private:
  std::vector<std::vector<std::byte>> sends_;
};

// Cell-direction (bits 22-23 == 0b10), exposed to the owning client.
constexpr uint32_t kCellRpcId = 0x00800701u;
uint32_t dispatched_rpcs = 0;

void RecordRpc(uint32_t, uint32_t, intptr_t, const uint8_t*, int32_t, uint64_t) {
  ++dispatched_rpcs;
}

auto ForwardedRpcs(const RecordingChannel& ch) -> std::vector<cellapp::ClientCellRpcForward> {
  std::vector<cellapp::ClientCellRpcForward> out;
  for (const auto& frame : ch.Sends()) {
    BinaryReader reader(std::span<const std::byte>(frame.data(), frame.size()));
    const auto id = reader.ReadPackedInt();
    if (!id || *id != cellapp::ClientCellRpcForward::Descriptor().id) continue;
    const auto len = reader.ReadPackedInt();
    if (!len) continue;
    const auto payload = reader.ReadBytes(*len);
    if (!payload) continue;
    BinaryReader mr(*payload);
    auto msg = cellapp::ClientCellRpcForward::Deserialize(mr);
    if (msg.HasValue()) out.push_back(std::move(*msg));
  }
  return out;
}

auto ReplyValues(const RecordingChannel& channel) -> std::vector<std::pair<uint64_t, int32_t>> {
  std::vector<std::pair<uint64_t, int32_t>> result;
  for (const auto& frame : channel.Sends()) {
    BinaryReader reader(frame);
    auto id = reader.ReadPackedInt();
    auto size = reader.ReadPackedInt();
    if (!id || !size || *id != EntityRpcReply::Descriptor().id) continue;
    auto request = reader.Read<uint64_t>();
    auto error = reader.Read<int32_t>();
    auto value = reader.Read<int32_t>();
    if (request && error && value && *error == 0) result.emplace_back(*request, *value);
  }
  return result;
}

}  // namespace

class BaseAppInflightRpcTest : public ::testing::Test {
 protected:
  BaseAppInflightRpcTest()
      : dispatcher_("baseapp_inflight"),
        internal_network_(dispatcher_),
        external_network_(dispatcher_),
        client_network_(dispatcher_),
        cell_addr_(0x7F000001u, 31000),
        app_(dispatcher_, internal_network_, external_network_),
        cell_ch_(make_intrusive<RecordingChannel>(dispatcher_, table_, cell_addr_)) {}

  void SetUp() override {
    dispatcher_.SetMaxPollWait(Milliseconds(1));
    EntityDefRegistry::Instance().clear();
    RegisterCellRpc();
    auto started = client_network_.StartRudpServer(Address("127.0.0.1", 0),
                                                   NetworkInterface::InternetRudpProfile());
    ASSERT_TRUE(started.HasValue()) << started.Error().Message();
    client_addr_ = client_network_.RudpAddress();
    client_ch_ = make_intrusive<RecordingChannel>(dispatcher_, table_, client_addr_);
  }

  void TearDown() override {
    dispatched_rpcs = 0;
    EntityDefRegistry::Instance().clear();
  }

  void EnableRpcDispatch() {
    native_provider_holder_ = app_.CreateNativeProvider();
    const struct {
      RestoreEntityFn restore;
      GetEntityDataFn get_data;
      EntityDestroyedFn destroyed;
      DispatchRpcFn dispatch;
    } callbacks{nullptr, nullptr, nullptr, &RecordRpc};
    app_.GetNativeProvider().SetNativeCallbacks(&callbacks, sizeof(callbacks));
  }

  void SendBaseRpc(uint32_t rpc_id) {
    baseapp::ClientBaseRpc msg;
    msg.rpc_id = rpc_id;
    app_.OnClientBaseRpc(*client_ch_, msg);
  }

  // Seed a client-bound entity that has NOT been placed on a cell yet, so
  // CellAddr().Port() == 0 and incoming cell RPCs buffer.
  auto SeedClientEntityWithoutCell() -> EntityID {
    app_.entity_mgr_.SetIdClient(&app_.id_client_);
    app_.id_client_.AddIds(1200, 1300);
    auto* entity = app_.entity_mgr_.Create(/*type_id=*/7, /*has_client=*/true);
    EXPECT_NE(entity, nullptr);
    app_.cellapp_peers_.InsertForTest(cell_addr_, cell_ch_.get());
    (void)external_network_.ConnectRudp(client_addr_);
    EXPECT_TRUE(app_.BindClient(entity->EntityId(), client_addr_));
    return entity->EntityId();
  }

  void SendCellRpc(EntityID target, std::byte tag, uint32_t rpc_id = kCellRpcId) {
    baseapp::ClientCellRpc msg;
    msg.target_entity_id = target;
    msg.rpc_id = rpc_id;
    msg.payload = {tag};
    app_.OnClientCellRpc(*client_ch_, msg);
  }

  void DeliverCurrentCell(EntityID entity_id, uint32_t epoch = 1) {
    DeliverCurrentCellFrom(entity_id, *cell_ch_, cell_addr_, epoch);
  }

  void DeliverCurrentCellFrom(EntityID entity_id, Channel& channel, const Address& address,
                              uint32_t epoch) {
    baseapp::CurrentCell cc;
    cc.entity_id = entity_id;
    cc.cell_addr = address;
    cc.epoch = epoch;
    app_.OnCurrentCell(channel, cc);
  }

  void ForwardReplyRequest(Channel& client, EntityID source,
                           const baseapp::ClientCellRpc& request) {
    app_.ForwardClientCellRpc(client, *cell_ch_, source, request);
  }

  auto BindOtherClient(EntityID entity_id, const Address& address) -> bool {
    return app_.BindClient(entity_id, address);
  }

  void TransferClient(EntityID source, EntityID destination) {
    app_.DoGiveClientToLocal(source, destination);
  }

  EventDispatcher dispatcher_;
  NetworkInterface internal_network_;
  NetworkInterface external_network_;
  NetworkInterface client_network_;
  InterfaceTable table_;
  Address client_addr_{0x7F000001u, 0};
  Address cell_addr_;
  BaseApp app_;
  std::unique_ptr<INativeApiProvider> native_provider_holder_;
  IntrusivePtr<RecordingChannel> cell_ch_;
  IntrusivePtr<RecordingChannel> client_ch_;

 private:
  static void RegisterCellRpc() {
    auto& reg = EntityDefRegistry::Instance();
    EntityTypeDescriptor type;
    type.type_id = 7;
    RpcDescriptor rpc;
    rpc.rpc_id = kCellRpcId;
    rpc.exposed = ExposedScope::kOwnClient;
    type.rpcs.push_back(std::move(rpc));
    reg.types.push_back(std::move(type));
    reg.rpc_to_type[kCellRpcId] = reg.types.size() - 1;
  }
};

TEST_F(BaseAppInflightRpcTest, CellRpcRejectsAnExposedComponentMethodFromAnotherEntityType) {
  const auto id = SeedClientEntityWithoutCell();
  DeliverCurrentCell(id);
  auto& registry = EntityDefRegistry::Instance();
  constexpr uint32_t kForeignRpc = (1u << 24) | 0x00800801u;
  EntityTypeDescriptor other;
  other.type_id = 8;
  RpcDescriptor rpc;
  rpc.rpc_id = kForeignRpc;
  rpc.exposed = ExposedScope::kOwnClient;
  other.rpcs.push_back(rpc);
  registry.rpc_to_type[kForeignRpc] = registry.types.size();
  registry.types.push_back(std::move(other));

  SendCellRpc(id, std::byte{0}, kForeignRpc);
  EXPECT_TRUE(ForwardedRpcs(*cell_ch_).empty());
  SendCellRpc(id, std::byte{1});
  EXPECT_EQ(ForwardedRpcs(*cell_ch_).size(), 1u);
}

TEST_F(BaseAppInflightRpcTest, BaseRpcRejectsAnExposedComponentMethodFromAnotherEntityType) {
  SeedClientEntityWithoutCell();
  EnableRpcDispatch();
  auto& registry = EntityDefRegistry::Instance();
  constexpr uint32_t kOwnRpc = (1u << 24) | 0x00C00701u;
  constexpr uint32_t kForeignRpc = (1u << 24) | 0x00C00801u;
  RpcDescriptor rpc;
  rpc.rpc_id = kOwnRpc;
  rpc.exposed = ExposedScope::kNone;
  registry.types[0].rpcs.push_back(rpc);
  registry.rpc_to_type[kOwnRpc] = 0;
  rpc.rpc_id = kForeignRpc;
  rpc.exposed = ExposedScope::kOwnClient;
  EntityTypeDescriptor other;
  other.type_id = 8;
  other.rpcs.push_back(rpc);
  registry.rpc_to_type[kForeignRpc] = registry.types.size();
  registry.types.push_back(std::move(other));

  SendBaseRpc(kForeignRpc);
  EXPECT_EQ(dispatched_rpcs, 0u);
  SendBaseRpc(kOwnRpc);
  EXPECT_EQ(dispatched_rpcs, 0u);
  registry.types[0].rpcs.back().exposed = ExposedScope::kOwnClient;
  SendBaseRpc(kOwnRpc);
  EXPECT_EQ(dispatched_rpcs, 1u);
}

TEST_F(BaseAppInflightRpcTest, BuffersDuringNoCellThenDrainsInOrderOnCurrentCell) {
  const EntityID eid = SeedClientEntityWithoutCell();
  ASSERT_NE(eid, kInvalidEntityID);

  // No cell yet: these three RPCs must buffer, not forward.
  SendCellRpc(eid, std::byte{1});
  SendCellRpc(eid, std::byte{2});
  SendCellRpc(eid, std::byte{3});
  EXPECT_TRUE(ForwardedRpcs(*cell_ch_).empty()) << "RPCs forwarded before the cell was known";

  // CurrentCell installs the address and drains the buffer in arrival order.
  DeliverCurrentCell(eid);

  auto forwarded = ForwardedRpcs(*cell_ch_);
  ASSERT_EQ(forwarded.size(), 3u);
  EXPECT_EQ(forwarded[0].payload.at(0), std::byte{1});
  EXPECT_EQ(forwarded[1].payload.at(0), std::byte{2});
  EXPECT_EQ(forwarded[2].payload.at(0), std::byte{3});
  for (const auto& f : forwarded) {
    EXPECT_EQ(f.target_entity_id, eid);
    EXPECT_EQ(f.source_entity_id, eid);
    EXPECT_EQ(f.rpc_id, kCellRpcId);
  }
}

TEST_F(BaseAppInflightRpcTest, StaleCurrentCellCannotDrainToTheOldHost) {
  const auto id = SeedClientEntityWithoutCell();
  SendCellRpc(id, std::byte{1});
  auto* entity = app_.GetEntityManager().Find(id);
  const Address current(0x7F000001u, 31001);
  entity->SetCell(current, 2);

  DeliverCurrentCell(id);

  EXPECT_EQ(entity->CellAddr(), current);
  EXPECT_TRUE(ForwardedRpcs(*cell_ch_).empty());
  DeliverCurrentCell(id, 3);
  ASSERT_EQ(ForwardedRpcs(*cell_ch_).size(), 1u);
}

TEST_F(BaseAppInflightRpcTest, CurrentCellRequiresRegisteredChannelAndMatchingAddress) {
  const auto id = SeedClientEntityWithoutCell();
  SendCellRpc(id, std::byte{1});
  auto* entity = app_.GetEntityManager().Find(id);
  auto impostor = make_intrusive<RecordingChannel>(dispatcher_, table_, cell_addr_);

  DeliverCurrentCellFrom(id, *impostor, cell_addr_, 1);
  DeliverCurrentCellFrom(id, *cell_ch_, Address(0x7F000001u, 31001), 1);

  EXPECT_FALSE(entity->HasCell());
  EXPECT_TRUE(ForwardedRpcs(*cell_ch_).empty());
  DeliverCurrentCell(id);
  EXPECT_EQ(ForwardedRpcs(*cell_ch_).size(), 1u);
}

TEST_F(BaseAppInflightRpcTest, EqualEpochCannotMoveCurrentCellToAnotherHost) {
  const auto id = SeedClientEntityWithoutCell();
  SendCellRpc(id, std::byte{1});
  auto* entity = app_.GetEntityManager().Find(id);
  const Address current(0x7F000001u, 31001);
  entity->SetCell(current, 2);

  DeliverCurrentCell(id, 2);

  EXPECT_EQ(entity->CellAddr(), current);
  EXPECT_TRUE(ForwardedRpcs(*cell_ch_).empty());
}

TEST_F(BaseAppInflightRpcTest, DestroyingEntityCannotAcquireCurrentCell) {
  const auto id = SeedClientEntityWithoutCell();
  auto* entity = app_.GetEntityManager().Find(id);
  entity->MarkForDestroy();

  DeliverCurrentCell(id);

  EXPECT_FALSE(entity->HasCell());
}

TEST_F(BaseAppInflightRpcTest, LocalTransferCannotDisplaceAnotherClient) {
  const auto src = SeedClientEntityWithoutCell();
  auto& manager = app_.GetEntityManager();
  auto* source = manager.FindProxy(src);
  auto* destination = dynamic_cast<Proxy*>(manager.Create(7, true));
  ASSERT_NE(destination, nullptr);
  const auto key = SessionKey::Generate();
  ASSERT_TRUE(key.HasValue());
  ASSERT_TRUE(manager.AssignSessionKey(src, *key));
  const Address other(0x7F000001u, 32001);
  ASSERT_TRUE(BindOtherClient(destination->EntityId(), other));

  TransferClient(src, destination->EntityId());

  EXPECT_TRUE(source->HasClient());
  EXPECT_EQ(source->ClientAddr(), client_addr_);
  EXPECT_EQ(manager.FindProxyBySession(*key), source);
  EXPECT_TRUE(destination->HasClient());
  EXPECT_EQ(destination->ClientAddr(), other);
}

TEST_F(BaseAppInflightRpcTest, LocalTransferRejectsDestroyingDestination) {
  const auto src = SeedClientEntityWithoutCell();
  auto& manager = app_.GetEntityManager();
  auto* source = manager.FindProxy(src);
  auto* destination = dynamic_cast<Proxy*>(manager.Create(7, true));
  ASSERT_NE(destination, nullptr);
  const auto key = SessionKey::Generate();
  ASSERT_TRUE(key.HasValue());
  ASSERT_TRUE(manager.AssignSessionKey(src, *key));
  destination->MarkForDestroy();

  TransferClient(src, destination->EntityId());

  EXPECT_TRUE(source->HasClient());
  EXPECT_EQ(manager.FindProxyBySession(*key), source);
  EXPECT_FALSE(destination->HasClient());
}

TEST_F(BaseAppInflightRpcTest, LocalTransferMovesBindingAndSessionTogether) {
  const auto src = SeedClientEntityWithoutCell();
  auto& manager = app_.GetEntityManager();
  auto* source = manager.FindProxy(src);
  auto* destination = dynamic_cast<Proxy*>(manager.Create(7, true));
  ASSERT_NE(destination, nullptr);
  const auto key = SessionKey::Generate();
  ASSERT_TRUE(key.HasValue());
  ASSERT_TRUE(manager.AssignSessionKey(src, *key));

  TransferClient(src, destination->EntityId());

  EXPECT_FALSE(source->HasClient());
  EXPECT_TRUE(source->GetSessionKey().IsZero());
  EXPECT_TRUE(destination->HasClient());
  EXPECT_EQ(destination->ClientAddr(), client_addr_);
  EXPECT_EQ(manager.FindProxyBySession(*key), destination);
}

TEST_F(BaseAppInflightRpcTest, TransferToAnAlreadyCreatedCellSendsReadyAfterIdentityChange) {
  const auto source = SeedClientEntityWithoutCell();
  auto& manager = app_.GetEntityManager();
  auto* destination = dynamic_cast<Proxy*>(manager.Create(7, true));
  ASSERT_NE(destination, nullptr);
  destination->SetCell(cell_addr_, 1);
  const auto key = SessionKey::Generate();
  ASSERT_TRUE(key.HasValue());
  ASSERT_TRUE(manager.AssignSessionKey(source, *key));
  std::vector<std::string> events;
  ASSERT_TRUE(client_network_.InterfaceTable().RegisterTypedHandler<baseapp::EntityTransferred>(
      [&](const Address&, Channel*, const baseapp::EntityTransferred& message) {
        EXPECT_EQ(message.new_entity_id, destination->EntityId());
        events.emplace_back("transferred");
      }));
  ASSERT_TRUE(client_network_.InterfaceTable().RegisterTypedHandler<baseapp::CellReady>(
      [&](const Address&, Channel*, const baseapp::CellReady& message) {
        EXPECT_EQ(message.entity_id, destination->EntityId());
        events.emplace_back("ready");
      }));
  TransferClient(source, destination->EntityId());
  const auto deadline = Clock::now() + std::chrono::seconds(2);
  while (events.size() < 2 && Clock::now() < deadline) dispatcher_.ProcessOnce();
  EXPECT_EQ(events, (std::vector<std::string>{"transferred", "ready"}));
}

TEST_F(BaseAppInflightRpcTest, LocalTransferPreservesPreparedDestinationSession) {
  const auto src = SeedClientEntityWithoutCell();
  auto& manager = app_.GetEntityManager();
  auto* source = manager.FindProxy(src);
  auto* destination = dynamic_cast<Proxy*>(manager.Create(7, true));
  ASSERT_NE(destination, nullptr);
  const auto source_key = SessionKey::Generate();
  const auto target_key = SessionKey::Generate();
  ASSERT_TRUE(source_key.HasValue());
  ASSERT_TRUE(target_key.HasValue());
  ASSERT_TRUE(manager.AssignSessionKey(src, *source_key));
  ASSERT_TRUE(manager.AssignSessionKey(destination->EntityId(), *target_key));

  TransferClient(src, destination->EntityId());

  EXPECT_TRUE(source->HasClient());
  EXPECT_FALSE(destination->HasClient());
  EXPECT_EQ(manager.FindProxyBySession(*source_key), source);
  EXPECT_EQ(manager.FindProxyBySession(*target_key), destination);
}

TEST_F(BaseAppInflightRpcTest, SameClientRequestIdsRelayIndependentlyAndRestoreOriginalIds) {
  auto other = make_intrusive<RecordingChannel>(dispatcher_, table_, Address(0x7F000001u, 32002));
  baseapp::ClientCellRpc request;
  request.target_entity_id = 1200;
  request.rpc_id = 0x80800001u;
  BinaryWriter payload;
  payload.Write(uint64_t{17});
  request.payload.assign(payload.Data().begin(), payload.Data().end());
  auto& registry = app_.GetRpcRegistry();
  const auto server_id = registry.AllocateRequestId();
  ForwardReplyRequest(*client_ch_, 1200, request);
  ForwardReplyRequest(*other, 1201, request);
  auto forwarded = ForwardedRpcs(*cell_ch_);
  ASSERT_EQ(forwarded.size(), 2u);
  BinaryReader first(forwarded[0].payload);
  BinaryReader second(forwarded[1].payload);
  auto first_id = first.Read<uint64_t>();
  auto second_id = second.Read<uint64_t>();
  ASSERT_TRUE(first_id);
  ASSERT_TRUE(second_id);
  EXPECT_NE(*first_id, *second_id);
  EXPECT_NE(*first_id, server_id);
  for (const auto& [id, value] : {std::pair{*second_id, 22}, std::pair{*first_id, 11}}) {
    BinaryWriter response;
    response.Write(id);
    response.Write(int32_t{0});
    response.Write(value);
    EXPECT_FALSE(
        registry.TryDispatch(EntityRpcReply::Descriptor().id, response.Data(), other.get()));
    EXPECT_TRUE(
        registry.TryDispatch(EntityRpcReply::Descriptor().id, response.Data(), cell_ch_.get()));
    EXPECT_FALSE(
        registry.TryDispatch(EntityRpcReply::Descriptor().id, response.Data(), cell_ch_.get()));
  }
  EXPECT_EQ(ReplyValues(*client_ch_), (std::vector<std::pair<uint64_t, int32_t>>{{17, 11}}));
  EXPECT_EQ(ReplyValues(*other), (std::vector<std::pair<uint64_t, int32_t>>{{17, 22}}));
  EXPECT_EQ(registry.PendingCount(), 0u);
  EXPECT_EQ(registry.ReplyCount(), 0u);
}

TEST_F(BaseAppInflightRpcTest, ClientDisconnectRemovesRelayAndLateCellReplyIsIgnored) {
  baseapp::ClientCellRpc request;
  request.target_entity_id = 1200;
  request.rpc_id = 0x80800001u;
  BinaryWriter payload;
  payload.Write(uint64_t{17});
  request.payload.assign(payload.Data().begin(), payload.Data().end());
  ForwardReplyRequest(*client_ch_, 1200, request);
  auto forwarded = ForwardedRpcs(*cell_ch_);
  ASSERT_EQ(forwarded.size(), 1u);
  BinaryReader forwarded_payload(forwarded[0].payload);
  auto forwarded_id = forwarded_payload.Read<uint64_t>();
  ASSERT_TRUE(forwarded_id);
  auto& registry = app_.GetRpcRegistry();
  registry.CancelByChannel(client_ch_.get());
  EXPECT_EQ(registry.PendingCount(), 0u);
  EXPECT_EQ(registry.ReplyCount(), 0u);
  BinaryWriter response;
  response.Write(*forwarded_id);
  response.Write(int32_t{0});
  EXPECT_FALSE(
      registry.TryDispatch(EntityRpcReply::Descriptor().id, response.Data(), cell_ch_.get()));
  EXPECT_TRUE(client_ch_->Sends().empty());
}

TEST_F(BaseAppInflightRpcTest, RelayPropagatesMalformedReplyCellDisconnectAndSendFailure) {
  baseapp::ClientCellRpc request;
  request.target_entity_id = 1200;
  request.rpc_id = 0x80800001u;
  BinaryWriter payload;
  payload.Write(uint64_t{17});
  request.payload.assign(payload.Data().begin(), payload.Data().end());
  auto& registry = app_.GetRpcRegistry();
  for (int failure = 0; failure < 3; ++failure) {
    auto client =
        make_intrusive<RecordingChannel>(dispatcher_, table_, Address(0x7F000001u, 32002));
    if (failure == 2) cell_ch_->Condemn();
    ForwardReplyRequest(*client, 1200, request);
    if (failure == 0) {
      auto forwarded = ForwardedRpcs(*cell_ch_);
      ASSERT_FALSE(forwarded.empty());
      BinaryReader reader(forwarded.back().payload);
      auto id = reader.Read<uint64_t>();
      ASSERT_TRUE(id);
      BinaryWriter malformed;
      malformed.Write(*id);
      ASSERT_TRUE(
          registry.TryDispatch(EntityRpcReply::Descriptor().id, malformed.Data(), cell_ch_.get()));
    } else if (failure == 1) {
      registry.CancelByChannel(cell_ch_.get());
    }
    ASSERT_EQ(client->Sends().size(), 1u);
    BinaryReader reply(client->Sends().front());
    ASSERT_TRUE(reply.ReadPackedInt());
    ASSERT_TRUE(reply.ReadPackedInt());
    auto original = reply.Read<uint64_t>();
    auto error = reply.Read<int32_t>();
    ASSERT_TRUE(original);
    ASSERT_TRUE(error);
    EXPECT_EQ(*original, 17u);
    EXPECT_EQ(*error, failure == 0 ? -7 : (failure == 1 ? -4 : -5));
    EXPECT_EQ(registry.PendingCount(), 0u);
    EXPECT_EQ(registry.ReplyCount(), 0u);
  }
}

TEST_F(BaseAppInflightRpcTest, RelayTimeoutRestoresClientIdAndRemovesBothRecords) {
  baseapp::ClientCellRpc request;
  request.target_entity_id = 1200;
  request.rpc_id = 0x80800001u;
  BinaryWriter payload;
  payload.Write(uint64_t{17});
  request.payload.assign(payload.Data().begin(), payload.Data().end());
  ForwardReplyRequest(*client_ch_, 1200, request);
  auto& registry = app_.GetRpcRegistry();
  const auto deadline = Clock::now() + std::chrono::seconds(12);
  while (registry.PendingCount() != 0 && Clock::now() < deadline) dispatcher_.ProcessOnce();
  EXPECT_EQ(registry.PendingCount(), 0u);
  EXPECT_EQ(registry.ReplyCount(), 0u);
  ASSERT_EQ(client_ch_->Sends().size(), 1u);
  BinaryReader response(client_ch_->Sends().front());
  ASSERT_TRUE(response.ReadPackedInt());
  ASSERT_TRUE(response.ReadPackedInt());
  auto id = response.Read<uint64_t>();
  auto error = response.Read<int32_t>();
  ASSERT_TRUE(id);
  ASSERT_TRUE(error);
  EXPECT_EQ(*id, 17u);
  EXPECT_EQ(*error, -1);
}

}  // namespace atlas
