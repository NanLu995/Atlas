#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "baseapp/baseapp_messages.h"
#include "baseapp/client_handoff_messages.h"
#include "coro/entity_rpc_reply.h"
#include "fake_cluster.h"
#include "net_client/client_api.h"
#include "net_client/client_session.h"
#include "network/channel.h"
#include "network/event_dispatcher.h"
#include "network/reliable_udp.h"
#include "serialization/binary_stream.h"

using namespace atlas;
using atlas::test::FakeCluster;

namespace atlas::net_client {

struct ClientSessionTestAccess {
  static void FillSendWindow(ClientSession& session, const Address& address, bool authenticate) {
    auto channel = session.network_.ConnectRudp(address);
    ASSERT_TRUE(channel);
    (*channel)->SetNocwnd(true);
    (*channel)->SetSendWindow(1);
    ASSERT_TRUE((*channel)->SendMessage(baseapp::Authenticate{}));
    ASSERT_EQ((*channel)->UnackedCount(), 1u);
    if (authenticate) {
      session.baseapp_addr_ = address;
      session.state_ = ATLAS_NET_STATE_LOGIN_SUCCEEDED;
    }
  }
  static auto Setup(ClientSession& session, Channel* channel) -> InterfaceTable& {
    session.baseapp_channel_ = channel;
    session.InstallDefaultHandler();
    return session.network_.InterfaceTable();
  }
  static void Clear(ClientSession& session) { session.baseapp_channel_ = nullptr; }
  static void Stop(ClientSession& session) { session.pending_disconnect_ = ATLAS_DISCONNECT_USER; }
  static void OldSourceAuth(ClientSession& session, const baseapp::AuthenticateResult& message) {
    BinaryWriter writer;
    message.Serialize(writer);
    BinaryReader reader(writer.Data());
    ASSERT_NE(session.handoff_source_channel_, nullptr);
    EXPECT_TRUE(session.network_.InterfaceTable().Dispatch({}, session.handoff_source_channel_,
                                                           message.Descriptor().id, reader));
  }
  static void ConnectSilentBase(ClientSession& session) {
    auto channel = session.network_.ConnectRudp(Address("127.0.0.1", 31003));
    ASSERT_TRUE(channel.HasValue());
    session.baseapp_channel_ = *channel;
    session.state_ = ATLAS_NET_STATE_CONNECTED;
    (*channel)->SetInactivityTimeout(std::chrono::milliseconds(1));
  }
};

}  // namespace atlas::net_client

namespace {

template <typename Pred>
bool PumpUntil(EventDispatcher& a, EventDispatcher& b, AtlasNetContext* ctx, Pred pred,
               std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    a.ProcessOnce();
    b.ProcessOnce();
    AtlasNetPoll(ctx);
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return false;
}

struct LoginCallbackState {
  std::atomic<bool> done{false};
  AtlasLoginStatus status{ATLAS_LOGIN_INTERNAL_ERROR};
  std::string baseapp_host;
  uint16_t baseapp_port{0};
};

struct AuthCallbackState {
  std::atomic<bool> done{false};
  bool success{false};
  uint32_t entity_id{0};
  uint16_t type_id{0};
  std::string error;
};

struct DeliverState {
  std::atomic<bool> movement_ack{false};
  uint16_t msg_id{0};
  std::vector<std::byte> payload;
};

DeliverState* g_deliver_state = nullptr;

struct DeliverStateScope {
  explicit DeliverStateScope(DeliverState& state) : previous(g_deliver_state) {
    g_deliver_state = &state;
  }

  ~DeliverStateScope() { g_deliver_state = previous; }

  DeliverState* previous;
};

void LoginCallback(void* user_data, uint8_t status, const char* baseapp_host, uint16_t baseapp_port,
                   const char* /*error*/) {
  auto* s = static_cast<LoginCallbackState*>(user_data);
  s->status = static_cast<AtlasLoginStatus>(status);
  if (baseapp_host) s->baseapp_host = baseapp_host;
  s->baseapp_port = baseapp_port;
  s->done = true;
}

void AuthCallback(void* user_data, uint8_t success, uint32_t entity_id, uint16_t type_id,
                  const char* error) {
  auto* s = static_cast<AuthCallbackState*>(user_data);
  s->success = success != 0;
  s->entity_id = entity_id;
  s->type_id = type_id;
  if (error) s->error = error;
  s->done = true;
}

void DeliverCallback(AtlasNetContext*, uint16_t msg_id, const uint8_t* payload, int32_t len) {
  if (g_deliver_state == nullptr || msg_id != baseapp::kClientMovementStateAckMessageId) return;
  auto& state = *g_deliver_state;
  state.msg_id = msg_id;
  state.payload.clear();
  if (payload != nullptr && len > 0) {
    const auto* begin = reinterpret_cast<const std::byte*>(payload);
    state.payload.assign(begin, begin + len);
  }
  state.movement_ack = true;
}

}  // namespace

TEST(NetClientFlow, ServerMessagesRequireBoundBaseAppChannel) {
  EventDispatcher dispatcher("reply_source");
  NetworkInterface network(dispatcher);
  auto expected = network.ConnectRudp(Address("127.0.0.1", 31001));
  auto wrong = network.ConnectRudp(Address("127.0.0.1", 31002));
  ASSERT_TRUE(expected.HasValue());
  ASSERT_TRUE(wrong.HasValue());
  net_client::ClientSession session;
  int deliveries = 0;
  static int* counter;
  counter = &deliveries;
  AtlasNetCallbacks callbacks{};
  callbacks.on_deliver = +[](AtlasNetContext*, uint16_t, const uint8_t*, int32_t) { ++*counter; };
  (void)session.SetCallbacks(callbacks);
  auto& table = net_client::ClientSessionTestAccess::Setup(session, *expected);
  auto check = [&](const auto& message) {
    BinaryWriter writer;
    message.Serialize(writer);
    auto dispatch = [&](Channel* channel) {
      BinaryReader reader(writer.Data());
      EXPECT_TRUE(table.Dispatch({}, channel, message.Descriptor().id, reader).HasValue());
    };
    const int before = deliveries;
    dispatch(*wrong);
    dispatch(nullptr);
    EXPECT_EQ(deliveries, before);
    dispatch(*expected);
    EXPECT_EQ(deliveries, before + 1);
  };
  check(EntityRpcReply{(uint64_t{1} << 32) + 17, 0, {}});
  check(baseapp::CellReady{42});
  check(baseapp::EntityTransferred{42, 7});
  net_client::ClientSessionTestAccess::Stop(session);
  BinaryWriter writer;
  EntityRpcReply reply{17, 0, {}};
  reply.Serialize(writer);
  BinaryReader reader(writer.Data());
  EXPECT_TRUE(table.Dispatch({}, *expected, reply.Descriptor().id, reader).HasValue());
  EXPECT_EQ(deliveries, 3);
  net_client::ClientSessionTestAccess::Clear(session);
  counter = nullptr;
}

TEST(NetClientFlow, LoginAndAuthenticateRejectFullSendWindowBeforeWaiting) {
  for (const bool authenticate : {false, true}) {
    net_client::ClientSession session;
    const Address address("127.0.0.1", 31004);
    net_client::ClientSessionTestAccess::FillSendWindow(session, address, authenticate);
    LoginCallbackState login;
    AuthCallbackState auth;
    const auto result = authenticate ? session.StartAuthenticate(AuthCallback, &auth)
                                     : session.StartLogin("127.0.0.1", 31004, "test-user",
                                                          "<test-password>", LoginCallback, &login);
    EXPECT_EQ(result, ATLAS_NET_ERR_BUSY);
    EXPECT_EQ(session.GetState(), ATLAS_NET_STATE_DISCONNECTED);
    EXPECT_FALSE(login.done.load());
    EXPECT_FALSE(auth.done.load());
    EXPECT_EQ(session.StartLogin("127.0.0.1", 31004, "test-user", "<test-password>", LoginCallback,
                                 &login),
              ATLAS_NET_OK);
    EXPECT_EQ(session.GetState(), ATLAS_NET_STATE_LOGGING_IN);
  }
}

class ClientHandoffFlowTest : public ::testing::Test {
 protected:
  void SetUp() override {
    current_ = this;
    ASSERT_TRUE(cluster_.Start());
    ASSERT_NE(cluster_.LoginAppPort(), cluster_.BaseAppPort());
    ASSERT_TRUE(target_network_.StartRudpServer(Address("127.0.0.1", 0)));
    ASSERT_TRUE(target_network_.InterfaceTable().RegisterTypedHandler<baseapp::Authenticate>(
        [this](const Address&, Channel* channel, const baseapp::Authenticate& request) {
          auth_channel_ = IntrusivePtr<Channel>{channel};
          authenticated_ = true;
          EXPECT_TRUE(request.session_key == credential_);
        }));
    ctx_ = AtlasNetCreate(ATLAS_NET_ABI_VERSION);
    ASSERT_NE(ctx_, nullptr);
    AtlasNetCallbacks callbacks{};
    callbacks.on_deliver = +[](AtlasNetContext*, uint16_t id, const uint8_t*, int32_t) {
      current_->messages_.push_back(id);
      if (current_->destroy_on_handoff_ && id == msg_id::Id(msg_id::BaseApp::kClientHandoff))
        AtlasNetDestroy(std::exchange(current_->ctx_, nullptr));
    };
    callbacks.on_disconnect = +[](AtlasNetContext*, int32_t) { ++current_->disconnects_; };
    ASSERT_EQ(AtlasNetSetCallbacks(ctx_, &callbacks), ATLAS_NET_OK);
    LoginCallbackState login;
    ASSERT_EQ(AtlasNetLogin(ctx_, "127.0.0.1", cluster_.LoginAppPort(), "handoff-test",
                            "<test-password>", &LoginCallback, &login),
              ATLAS_NET_OK);
    ASSERT_TRUE(PumpUntil(cluster_.LoginAppDisp(), cluster_.BaseAppDisp(), ctx_,
                          [&] { return login.done.load(); }));
    ASSERT_EQ(login.status, ATLAS_LOGIN_SUCCESS)
        << "login_port=" << cluster_.LoginAppPort() << " base_port=" << cluster_.BaseAppPort()
        << " error=" << AtlasNetLastError(ctx_);
    AuthCallbackState auth;
    ASSERT_EQ(AtlasNetAuthenticate(ctx_, &AuthCallback, &auth), ATLAS_NET_OK)
        << "login_port=" << cluster_.LoginAppPort() << " base_port=" << cluster_.BaseAppPort()
        << " state=" << AtlasNetGetState(ctx_) << " error=" << AtlasNetLastError(ctx_);
    ASSERT_TRUE(PumpUntil(cluster_.LoginAppDisp(), cluster_.BaseAppDisp(), ctx_,
                          [&] { return auth.done.load(); }))
        << "login_port=" << cluster_.LoginAppPort() << " base_port=" << cluster_.BaseAppPort()
        << " request_seen=" << cluster_.AuthenticateRequestSeen()
        << " state=" << AtlasNetGetState(ctx_) << " error=" << AtlasNetLastError(ctx_);
    ASSERT_TRUE(auth.success) << "error=" << auth.error << " login_port=" << cluster_.LoginAppPort()
                              << " base_port=" << cluster_.BaseAppPort()
                              << " request_seen=" << cluster_.AuthenticateRequestSeen();
    auto credential = SessionKey::Generate();
    ASSERT_TRUE(credential.HasValue());
    credential_ = *credential;
  }
  void TearDown() override {
    AtlasNetDestroy(ctx_);
    current_ = nullptr;
  }
  template <typename Predicate>
  auto Pump(Predicate predicate, int milliseconds = 2000) -> bool {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
      cluster_.BaseAppDisp().ProcessOnce();
      target_dispatcher_.ProcessOnce();
      if (ctx_) (void)AtlasNetPoll(ctx_);
    }
    return predicate();
  }
  void Redirect() {
    baseapp::ClientHandoff request;
    request.target = {target_network_.RudpAddress(), 55, 222};
    request.credential = credential_;
    ASSERT_TRUE(cluster_.PushClientHandoff(request));
  }
  void Confirm(uint64_t instance = 222) {
    ASSERT_TRUE(auth_channel_);
    baseapp::AuthenticateResult result;
    result.success = true;
    result.entity_id = 55;
    result.type_id = 7;
    result.process_instance = instance;
    ASSERT_TRUE(auth_channel_->SendMessage(result));
    ASSERT_TRUE(auth_channel_->SendMessage(baseapp::CellReady{55}));
  }
  inline static ClientHandoffFlowTest* current_{nullptr};
  FakeCluster cluster_;
  EventDispatcher target_dispatcher_{"handoff_target"};
  NetworkInterface target_network_{target_dispatcher_};
  IntrusivePtr<Channel> auth_channel_;
  AtlasNetContext* ctx_{nullptr};
  SessionKey credential_;
  std::vector<uint16_t> messages_;
  int disconnects_{0};
  bool authenticated_{false};
  bool destroy_on_handoff_{false};
};

TEST_F(ClientHandoffFlowTest, SwitchRequiresTheTargetReplyAndTransfersBeforeCellReady) {
  Redirect();
  ASSERT_TRUE(Pump([&] { return authenticated_; }));
  EXPECT_EQ(AtlasNetGetState(ctx_), ATLAS_NET_STATE_HANDOFF);
  EXPECT_EQ(AtlasNetSendBaseRpc(ctx_, 42, 1, nullptr, 0), ATLAS_NET_ERR_NOCONN);
  EXPECT_EQ(AtlasNetSendCellRpc(ctx_, 42, 1, nullptr, 0), ATLAS_NET_ERR_NOCONN);
  baseapp::AuthenticateResult old;
  old.success = true;
  old.entity_id = 55;
  old.type_id = 7;
  old.process_instance = 222;
  net_client::ClientSessionTestAccess::OldSourceAuth(*ctx_, old);
  EXPECT_EQ(AtlasNetGetState(ctx_), ATLAS_NET_STATE_HANDOFF);
  Confirm();
  ASSERT_TRUE(Pump([&] { return messages_.size() == 3; }));
  EXPECT_EQ(AtlasNetGetState(ctx_), ATLAS_NET_STATE_CONNECTED);
  const std::vector<uint16_t> expected{msg_id::Id(msg_id::BaseApp::kClientHandoff),
                                       msg_id::Id(msg_id::BaseApp::kEntityTransferred),
                                       msg_id::Id(msg_id::BaseApp::kCellReady)};
  EXPECT_EQ(messages_, expected);
  EXPECT_EQ(disconnects_, 0);
}

TEST_F(ClientHandoffFlowTest, WrongTargetProcessFailsWithoutResumingTheOldConnection) {
  Redirect();
  ASSERT_TRUE(Pump([&] { return authenticated_; }));
  Confirm(333);
  ASSERT_TRUE(Pump([&] { return disconnects_ != 0; }));
  EXPECT_EQ(disconnects_, 1);
  EXPECT_EQ(AtlasNetGetState(ctx_), ATLAS_NET_STATE_DISCONNECTED);
  EXPECT_EQ(messages_.size(), 1u);
  EXPECT_NE(std::string(AtlasNetLastError(ctx_)).find("mismatch"), std::string::npos);
  (void)AtlasNetPoll(ctx_);
  EXPECT_EQ(disconnects_, 1);
}

TEST_F(ClientHandoffFlowTest, HandoffTimeoutDisconnectsAndDoesNotGuessSuccess) {
  Redirect();
  ASSERT_TRUE(Pump([&] { return authenticated_; }));
  ASSERT_TRUE(Pump([&] { return disconnects_ != 0; }, 7000));
  EXPECT_EQ(AtlasNetGetState(ctx_), ATLAS_NET_STATE_DISCONNECTED);
  EXPECT_EQ(messages_.size(), 1u);
  EXPECT_NE(std::string(AtlasNetLastError(ctx_)).find("timeout"), std::string::npos);
}

TEST_F(ClientHandoffFlowTest, DestroyDuringHandoffNotificationDefersUntilPollReturns) {
  destroy_on_handoff_ = true;
  Redirect();
  ASSERT_TRUE(Pump([&] { return ctx_ == nullptr; }));
  EXPECT_EQ(messages_.size(), 1u);
}

TEST(NetClientFlow, LostBaseChannelNotifiesDisconnectExactlyOnce) {
  auto* ctx = AtlasNetCreate(ATLAS_NET_ABI_VERSION);
  ASSERT_NE(ctx, nullptr);
  static int notifications;
  notifications = 0;
  AtlasNetCallbacks callbacks{};
  callbacks.on_disconnect = +[](AtlasNetContext*, int32_t) { ++notifications; };
  (void)AtlasNetSetCallbacks(ctx, &callbacks);
  net_client::ClientSessionTestAccess::ConnectSilentBase(*ctx);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (notifications == 0 && std::chrono::steady_clock::now() < deadline) {
    (void)AtlasNetPoll(ctx);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_EQ(notifications, 1);
  EXPECT_EQ(AtlasNetGetState(ctx), ATLAS_NET_STATE_DISCONNECTED);
  EXPECT_EQ(AtlasNetSendBaseRpc(ctx, 1, 1, nullptr, 0), ATLAS_NET_ERR_NOCONN);
  (void)AtlasNetPoll(ctx);
  EXPECT_EQ(notifications, 1);
  AtlasNetDestroy(ctx);
}

TEST(NetClientFlow, EndToEndCreateLoginAuthSendDisconnect) {
  FakeCluster cluster;
  ASSERT_TRUE(cluster.Start()) << "fake cluster failed to bind";

  AtlasNetContext* ctx = AtlasNetCreate(ATLAS_NET_ABI_VERSION);
  ASSERT_NE(ctx, nullptr);
  ASSERT_EQ(AtlasNetGetState(ctx), ATLAS_NET_STATE_DISCONNECTED);

  DeliverState deliver_state;
  DeliverStateScope deliver_scope(deliver_state);
  AtlasNetCallbacks cbs{};
  cbs.on_deliver = &DeliverCallback;
  ASSERT_EQ(AtlasNetSetCallbacks(ctx, &cbs), ATLAS_NET_OK);

  LoginCallbackState login_state;
  ASSERT_EQ(ATLAS_NET_OK, AtlasNetLogin(ctx, "127.0.0.1", cluster.LoginAppPort(), "alice",
                                        "pwd-hash", &LoginCallback, &login_state));

  ASSERT_TRUE(PumpUntil(cluster.LoginAppDisp(), cluster.BaseAppDisp(), ctx, [&] {
    return login_state.done.load();
  })) << "login callback never fired";
  EXPECT_TRUE(cluster.LoginRequestSeen());
  EXPECT_EQ(login_state.status, ATLAS_LOGIN_SUCCESS);
  EXPECT_EQ(AtlasNetGetState(ctx), ATLAS_NET_STATE_LOGIN_SUCCEEDED);

  AuthCallbackState auth_state;
  ASSERT_EQ(ATLAS_NET_OK, AtlasNetAuthenticate(ctx, &AuthCallback, &auth_state));

  ASSERT_TRUE(PumpUntil(cluster.LoginAppDisp(), cluster.BaseAppDisp(), ctx, [&] {
    return auth_state.done.load();
  })) << "authenticate callback never fired";
  EXPECT_TRUE(cluster.AuthenticateRequestSeen());
  EXPECT_TRUE(auth_state.success);
  EXPECT_EQ(auth_state.entity_id, 42u);
  EXPECT_EQ(auth_state.type_id, 7);
  EXPECT_EQ(AtlasNetGetState(ctx), ATLAS_NET_STATE_CONNECTED);

  ASSERT_TRUE(cluster.PushMovementStateAck(42, 7, 1234));
  ASSERT_TRUE(PumpUntil(cluster.LoginAppDisp(), cluster.BaseAppDisp(), ctx, [&] {
    return deliver_state.movement_ack.load();
  })) << "movement ack was not delivered";
  EXPECT_EQ(deliver_state.msg_id, baseapp::kClientMovementStateAckMessageId);
  EXPECT_EQ(deliver_state.payload.size(),
            static_cast<std::size_t>(baseapp::MovementStateAckToClient::Descriptor().fixed_length));
  BinaryReader ack_reader(deliver_state.payload);
  auto ack = baseapp::MovementStateAckToClient::Deserialize(ack_reader);
  ASSERT_TRUE(ack.HasValue());
  EXPECT_EQ(ack->entity_id, 42u);
  EXPECT_EQ(ack->acked_input_seq, 7u);
  EXPECT_EQ(ack->server_tick, 1234u);
  EXPECT_EQ(ack->state.position.x, 1.0f);
  EXPECT_EQ(ack->state.velocity.z, 6.0f);
  EXPECT_EQ(ack->correction_flags, movement::kCorrectionFlagTier2);

  const uint8_t payload[] = {0xDE, 0xAD, 0xBE, 0xEF};
  EXPECT_EQ(ATLAS_NET_OK, AtlasNetSendBaseRpc(ctx, /*entity_id=*/42, /*rpc_id=*/0x1234, payload,
                                              static_cast<int32_t>(sizeof(payload))));

  ASSERT_TRUE(PumpUntil(cluster.LoginAppDisp(), cluster.BaseAppDisp(), ctx, [&] {
    return cluster.RpcReceived();
  })) << "BaseApp never saw the RPC";
  EXPECT_EQ(cluster.LastRpcId(), 0x1234u);
  ASSERT_EQ(cluster.LastRpcPayload().size(), sizeof(payload));
  EXPECT_EQ(0, std::memcmp(cluster.LastRpcPayload().data(), payload, sizeof(payload)));

  EXPECT_EQ(ATLAS_NET_OK, AtlasNetDisconnect(ctx, ATLAS_DISCONNECT_USER));
  EXPECT_EQ(AtlasNetGetState(ctx), ATLAS_NET_STATE_DISCONNECTED);

  AtlasNetDestroy(ctx);
}

TEST(NetClientFlow, LoginToInvalidPortTimesOut) {
  AtlasNetContext* ctx = AtlasNetCreate(ATLAS_NET_ABI_VERSION);
  ASSERT_NE(ctx, nullptr);

  AtlasNetCallbacks cbs{};
  AtlasNetSetCallbacks(ctx, &cbs);

  LoginCallbackState login_state;
  EXPECT_EQ(ATLAS_NET_OK, AtlasNetLogin(ctx, "127.0.0.1", /*unused port*/ 1u, "alice", "pwd",
                                        &LoginCallback, &login_state));

  // Pump only the ctx; no fake cluster means the LoginApp connection times out.
  // Keep this short and only assert that the state machine entered LoggingIn.
  for (int i = 0; i < 100; ++i) {
    AtlasNetPoll(ctx);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    if (login_state.done.load()) break;
  }
  EXPECT_EQ(AtlasNetGetState(ctx), ATLAS_NET_STATE_LOGGING_IN);

  EXPECT_EQ(ATLAS_NET_OK, AtlasNetDisconnect(ctx, ATLAS_DISCONNECT_USER));
  AtlasNetDestroy(ctx);
}

TEST(NetClientFlow, DestroyDuringLoginCallbackWaitsForPollToReturn) {
  FakeCluster cluster;
  ASSERT_TRUE(cluster.Start());
  auto* ctx = AtlasNetCreate(ATLAS_NET_ABI_VERSION);
  ASSERT_NE(ctx, nullptr);
  struct State {
    AtlasNetContext* context;
    bool completed{false};
    int32_t nested_poll{ATLAS_NET_OK};
  } state{ctx};
  auto callback = +[](void* data, uint8_t, const char*, uint16_t, const char*) {
    auto& current = *static_cast<State*>(data);
    current.nested_poll = AtlasNetPoll(current.context);
    AtlasNetDestroy(current.context);
    AtlasNetDestroy(current.context);
    current.completed = true;
  };
  ASSERT_EQ(AtlasNetLogin(ctx, "127.0.0.1", cluster.LoginAppPort(), "callback_destroy",
                          "<test-password>", callback, &state),
            ATLAS_NET_OK);
  EXPECT_TRUE(PumpUntil(cluster.LoginAppDisp(), cluster.BaseAppDisp(), ctx,
                        [&] { return state.completed; }));
  EXPECT_EQ(state.nested_poll, ATLAS_NET_ERR_BUSY);
  if (!state.completed) AtlasNetDestroy(ctx);
}

TEST(NetClientFlow, DisconnectDuringLoginCallbackCanReconnectAfterPoll) {
  FakeCluster cluster;
  ASSERT_TRUE(cluster.Start());
  auto* ctx = AtlasNetCreate(ATLAS_NET_ABI_VERSION);
  ASSERT_NE(ctx, nullptr);
  struct State {
    AtlasNetContext* context;
    bool completed{false};
    int32_t result{ATLAS_NET_ERR_INVAL};
  } state{ctx};
  auto callback = +[](void* data, uint8_t, const char*, uint16_t, const char*) {
    auto& current = *static_cast<State*>(data);
    current.result = AtlasNetDisconnect(current.context, ATLAS_DISCONNECT_USER);
    current.completed = true;
  };
  ASSERT_EQ(AtlasNetLogin(ctx, "127.0.0.1", cluster.LoginAppPort(), "callback_disconnect",
                          "<test-password>", callback, &state),
            ATLAS_NET_OK);
  ASSERT_TRUE(PumpUntil(cluster.LoginAppDisp(), cluster.BaseAppDisp(), ctx,
                        [&] { return state.completed; }));
  EXPECT_EQ(state.result, ATLAS_NET_OK);
  EXPECT_EQ(AtlasNetGetState(ctx), ATLAS_NET_STATE_DISCONNECTED);
  LoginCallbackState reconnected;
  EXPECT_EQ(AtlasNetLogin(ctx, "127.0.0.1", cluster.LoginAppPort(), "callback_reconnect",
                          "<test-password>", &LoginCallback, &reconnected),
            ATLAS_NET_OK);
  EXPECT_TRUE(PumpUntil(cluster.LoginAppDisp(), cluster.BaseAppDisp(), ctx,
                        [&] { return reconnected.done.load(); }));
  AtlasNetDestroy(ctx);
}

TEST(NetClientFlow, DestroyFromLogoutCallbackCompletesOuterDisconnect) {
  FakeCluster cluster;
  ASSERT_TRUE(cluster.Start());
  auto* ctx = AtlasNetCreate(ATLAS_NET_ABI_VERSION);
  ASSERT_NE(ctx, nullptr);
  LoginCallbackState login;
  ASSERT_EQ(AtlasNetLogin(ctx, "127.0.0.1", cluster.LoginAppPort(), "logout_destroy",
                          "<test-password>", &LoginCallback, &login),
            ATLAS_NET_OK);
  ASSERT_TRUE(PumpUntil(cluster.LoginAppDisp(), cluster.BaseAppDisp(), ctx,
                        [&] { return login.done.load(); }));
  AuthCallbackState auth;
  ASSERT_EQ(AtlasNetAuthenticate(ctx, &AuthCallback, &auth), ATLAS_NET_OK);
  ASSERT_TRUE(PumpUntil(cluster.LoginAppDisp(), cluster.BaseAppDisp(), ctx,
                        [&] { return auth.done.load(); }));
  AtlasNetCallbacks callbacks{};
  callbacks.on_disconnect = +[](AtlasNetContext* context, int32_t) { AtlasNetDestroy(context); };
  ASSERT_EQ(AtlasNetSetCallbacks(ctx, &callbacks), ATLAS_NET_OK);
  EXPECT_EQ(AtlasNetDisconnect(ctx, ATLAS_DISCONNECT_LOGOUT), ATLAS_NET_OK);
}

TEST(NetClientFlow, DisconnectDuringAuthCallbackRejectsFurtherSends) {
  FakeCluster cluster;
  ASSERT_TRUE(cluster.Start());
  auto* ctx = AtlasNetCreate(ATLAS_NET_ABI_VERSION);
  ASSERT_NE(ctx, nullptr);
  LoginCallbackState login;
  ASSERT_EQ(AtlasNetLogin(ctx, "127.0.0.1", cluster.LoginAppPort(), "auth_disconnect",
                          "<test-password>", &LoginCallback, &login),
            ATLAS_NET_OK);
  ASSERT_TRUE(PumpUntil(cluster.LoginAppDisp(), cluster.BaseAppDisp(), ctx,
                        [&] { return login.done.load(); }));
  struct State {
    AtlasNetContext* context;
    bool completed{false};
    int32_t base_result{ATLAS_NET_OK};
    int32_t cell_result{ATLAS_NET_OK};
  } state{ctx};
  auto callback = +[](void* data, uint8_t, uint32_t, uint16_t, const char*) {
    auto& current = *static_cast<State*>(data);
    (void)AtlasNetDisconnect(current.context, ATLAS_DISCONNECT_USER);
    current.base_result = AtlasNetSendBaseRpc(current.context, 42, 1, nullptr, 0);
    current.cell_result = AtlasNetSendCellRpc(current.context, 42, 1, nullptr, 0);
    current.completed = true;
  };
  ASSERT_EQ(AtlasNetAuthenticate(ctx, callback, &state), ATLAS_NET_OK);
  EXPECT_TRUE(PumpUntil(cluster.LoginAppDisp(), cluster.BaseAppDisp(), ctx,
                        [&] { return state.completed; }));
  EXPECT_EQ(state.base_result, ATLAS_NET_ERR_NOCONN);
  EXPECT_EQ(state.cell_result, ATLAS_NET_ERR_NOCONN);
  EXPECT_EQ(AtlasNetGetState(ctx), ATLAS_NET_STATE_DISCONNECTED);
  AtlasNetDestroy(ctx);
}
