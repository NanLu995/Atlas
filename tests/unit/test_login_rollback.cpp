#include <algorithm>
#include <array>
#include <format>
#include <initializer_list>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "baseapp/baseapp.h"
#include "cellappmgr/cellappmgr_messages.h"
#include "coro/cancellation.h"
#include "coro/fire_and_forget.h"
#include "coro/rpc_call.h"
#include "db/entity_blob.h"
#include "db/idatabase.h"
#include "dbapp/dbapp.h"
#include "dbappmgr/dbappmgr_messages.h"
#include "foundation/intrusive_ptr.h"
#include "loginapp/loginapp.h"
#include "network/reliable_udp.h"
#include "platform/password_hash.h"
#include "serialization/binary_stream.h"
#include "server/db_shard_routing.h"
#include "test_null_channel.h"
#include "test_shutdown_access.h"

namespace atlas {

namespace {

auto DatabaseBlob(std::initializer_list<std::byte> base = {},
                  std::initializer_list<std::byte> cell = {}) -> std::vector<std::byte> {
  auto encoded =
      EncodeDbBlob(std::span(base.begin(), base.size()), std::span(cell.begin(), cell.size()));
  EXPECT_TRUE(encoded.HasValue());
  return encoded ? std::move(*encoded) : std::vector<std::byte>{};
}

class CapturingChannel final : public Channel {
 public:
  using Channel::Channel;

  [[nodiscard]] auto Fd() const -> FdHandle override { return kInvalidFd; }
  [[nodiscard]] auto send_count() const -> std::size_t { return sends_.size(); }
  [[nodiscard]] auto Sends() const -> const std::vector<std::vector<std::byte>>& { return sends_; }
  void FailNextSend() { fail_next_send_ = true; }

 protected:
  [[nodiscard]] auto DoSend(std::span<const std::byte> data) -> Result<std::size_t> override {
    if (fail_next_send_) {
      fail_next_send_ = false;
      return Error{ErrorCode::kInternalError, "injected send failure"};
    }
    sends_.emplace_back(data.begin(), data.end());
    return data.size();
  }

 private:
  std::vector<std::vector<std::byte>> sends_;
  bool fail_next_send_{false};
};

template <typename Msg>
auto DecodeSentMessages(const CapturingChannel& ch) -> std::vector<Msg> {
  std::vector<Msg> out;
  for (const auto& frame : ch.Sends()) {
    BinaryReader reader(std::span<const std::byte>(frame.data(), frame.size()));
    const auto id = reader.ReadPackedInt();
    if (!id || *id != Msg::Descriptor().id) continue;

    if (Msg::Descriptor().length_style == MessageLengthStyle::kVariable) {
      const auto len = reader.ReadPackedInt();
      if (!len) continue;
      const auto payload = reader.ReadBytes(*len);
      if (!payload) continue;
      BinaryReader msg_reader(*payload);
      auto msg = Msg::Deserialize(msg_reader);
      if (msg.HasValue()) out.push_back(std::move(*msg));
      continue;
    }

    auto msg = Msg::Deserialize(reader);
    if (msg.HasValue()) out.push_back(std::move(*msg));
  }
  return out;
}

}  // namespace

class LoginRollbackTest : public ::testing::Test {
 protected:
  LoginRollbackTest()
      : dispatcher_("login_rollback"),
        internal_network_(dispatcher_),
        external_network_(dispatcher_),
        app_(dispatcher_, internal_network_, external_network_) {}

  void register_watchers() { app_.RegisterWatchers(); }

  // Seed an active login's cancellation state (simulates what handle_login_coro sets up)
  auto seed_active_login(uint64_t channel_id, const std::string& username, uint32_t request_id)
      -> CancellationToken {
    CancellationSource source;
    auto token = source.Token();
    app_.channel_cancel_sources_[channel_id] = source;
    app_.pending_by_username_[username] = request_id;
    return token;
  }

  void cancel_for_channel(uint64_t channel_id) {
    auto it = app_.channel_cancel_sources_.find(channel_id);
    if (it != app_.channel_cancel_sources_.end()) it->second.RequestCancellation();
  }

  void add_pending_username(const std::string& username, uint32_t request_id) {
    app_.pending_by_username_[username] = request_id;
  }

  void remove_pending_username(const std::string& username) {
    app_.pending_by_username_.erase(username);
  }

  auto pending_by_username_empty() const -> bool { return app_.pending_by_username_.empty(); }
  auto active_login_count() const -> std::size_t { return app_.channel_cancel_sources_.size(); }
  auto abandoned_total() const -> uint64_t { return app_.abandoned_login_total_; }
  auto watcher(std::string_view path) -> std::optional<std::string> {
    return app_.GetWatcherRegistry().Get(path);
  }
  void set_legacy_dbapp_channel(Channel* ch) { app_.dbapp_channel_ = ch; }
  void set_baseappmgr_channel(Channel* ch) { app_.baseappmgr_channel_ = ch; }
  void cache_dbapp_channel(const Address& addr, Channel* ch) { app_.dbapp_channels_[addr] = ch; }
  auto cached_dbapp_channel_count() const -> std::size_t { return app_.dbapp_channels_.size(); }
  void apply_dbapp_shards(uint32_t version, std::vector<dbappmgr::ShardEntry> entries) {
    app_.ApplyDbAppShardTable(version, std::move(entries));
  }
  auto resolve_auth_dbapp(std::string_view username) -> Channel* {
    return app_.ResolveAuthDbAppChannel(username);
  }
  void handle_dbapp_death(const Address& addr) {
    machined::DeathNotification death;
    death.process_type = ProcessType::kDbApp;
    death.internal_addr = addr;
    death.reason = 1;
    app_.OnDbAppDeath(death);
  }
  auto start_login(uint64_t client_channel_id, std::string username) -> uint32_t {
    login::LoginRequest request;
    request.username = std::move(username);
    request.password_hash = "pw";
    const uint32_t request_id = app_.next_request_id_;
    app_.HandleLoginCoro(client_channel_id, Address(0x7F000001u, 26001), std::move(request));
    return request_id;
  }
  auto connect_login_client(const Address& address) -> Result<ReliableUdpChannel*> {
    external_network_.SetDisconnectCallback(
        [this](Channel& channel) { app_.OnClientDisconnect(channel); });
    auto result = external_network_.ConnectRudp(address);
    if (result) (*result)->SetInactivityTimeout(login_idle_timeout());
    return result;
  }
  static auto login_idle_timeout() -> Duration {
    return std::chrono::seconds(LoginApp::kClientChannelInactivitySec);
  }
  void disconnect_login_client(Channel& channel) {
    external_network_.DisconnectChannel(channel.RemoteAddress());
    app_.OnClientDisconnect(channel);
  }
  auto login_channel_exists(uint64_t id) -> bool {
    return external_network_.FindChannel(id) != nullptr;
  }
  auto pending_rpc_count() const -> std::size_t { return app_.rpc_registry_.PendingCount(); }
  void request_resume(Channel& channel, DatabaseID dbid) {
    login::ResumeRequest request;
    request.credential = {dbid, 7, {}, 601000};
    request.credential.value[0] = std::byte{1};
    app_.OnResumeRequest(channel.RemoteAddress(), &channel, request);
  }
  auto pending_recoveries() const -> std::size_t { return app_.pending_recoveries_.size(); }
  auto handle_auth_result(Channel* channel, const login::AuthLoginResult& result) -> bool {
    BinaryWriter writer;
    result.Serialize(writer);
    return app_.rpc_registry_.TryDispatch(login::AuthLoginResult::Descriptor().id, writer.Data(),
                                          channel);
  }
  template <typename Predicate>
  auto drive_until(Predicate predicate, Duration timeout) -> bool {
    const auto deadline = Clock::now() + timeout;
    while (Clock::now() < deadline) {
      dispatcher_.ProcessOnce();
      if (predicate()) return true;
    }
    dispatcher_.ProcessOnce();
    return predicate();
  }

  EventDispatcher dispatcher_;
  NetworkInterface internal_network_;
  NetworkInterface external_network_;
  LoginApp app_;
};

TEST_F(LoginRollbackTest, ReadinessReportsLocalDependenciesWithoutIssuingWork) {
  register_watchers();
  EXPECT_EQ(watcher("app/ready"), "false");
  EXPECT_EQ(watcher("app/not_ready_reason"), "no_database_route");
  InterfaceTable table;
  CapturingChannel db_channel(dispatcher_, table, Address("127.0.0.1", 24001));
  CapturingChannel manager_channel(dispatcher_, table, Address("127.0.0.1", 24002));
  set_legacy_dbapp_channel(&db_channel);
  EXPECT_EQ(watcher("app/not_ready_reason"), "no_baseappmgr");
  set_baseappmgr_channel(&manager_channel);
  for (int i = 0; i < 5; ++i) {
    EXPECT_EQ(watcher("app/ready"), "true");
    EXPECT_EQ(watcher("app/not_ready_reason"), "");
  }
  EXPECT_EQ(db_channel.send_count(), 0u);
  EXPECT_EQ(manager_channel.send_count(), 0u);
  set_legacy_dbapp_channel(nullptr);
  set_baseappmgr_channel(nullptr);
}

TEST_F(LoginRollbackTest, RecoverySharesAdmissionMetricsAndCancelsOnDisconnect) {
  register_watchers();
  NetworkInterface client(dispatcher_);
  ASSERT_TRUE(client.StartRudpServer(Address("127.0.0.1", 0)));
  auto connected = connect_login_client(Address("127.0.0.1", client.RudpAddress().Port()));
  ASSERT_TRUE(connected);
  IntrusivePtr<Channel> channel{*connected};
  InterfaceTable table;
  auto manager = make_intrusive<CapturingChannel>(dispatcher_, table, Address("127.0.0.1", 24002));
  set_baseappmgr_channel(manager.get());
  request_resume(*channel, 42);
  EXPECT_EQ(pending_recoveries(), 1u);
  EXPECT_EQ(active_login_count(), 1u);
  EXPECT_EQ(watcher("loginapp/pending_logins"), "1");
  const auto allocations = DecodeSentMessages<login::AllocateBaseApp>(*manager);
  ASSERT_EQ(allocations.size(), 1u);
  EXPECT_EQ(allocations.front().dbid, 42);
  EXPECT_EQ(allocations.front().type_id, 7);
  request_resume(*channel, 42);
  EXPECT_EQ(manager->send_count(), 1u);
  disconnect_login_client(*channel);
  EXPECT_EQ(pending_recoveries(), 0u);
  EXPECT_EQ(pending_rpc_count(), 0u);
  EXPECT_EQ(active_login_count(), 0u);
  EXPECT_EQ(watcher("loginapp/pending_logins"), "0");
}

TEST_F(LoginRollbackTest, ClientDisconnectCancelsActiveLogin) {
  register_watchers();
  auto token = seed_active_login(100, "tester", 42);

  EXPECT_EQ(active_login_count(), 1u);
  EXPECT_FALSE(pending_by_username_empty());
  EXPECT_FALSE(token.IsCancelled());

  // Simulate what on_client_disconnect does: cancel the source
  cancel_for_channel(100);

  EXPECT_TRUE(token.IsCancelled());
}

TEST_F(LoginRollbackTest, PendingLoginSurvivesIdleGraceWhileBackendReplyIsPending) {
  NetworkInterface client(dispatcher_);
  ASSERT_TRUE(client.StartRudpServer(Address("127.0.0.1", 0)));
  bool received = false;
  login::LoginStatus status = login::LoginStatus::kSuccess;
  ASSERT_TRUE(client.InterfaceTable().RegisterTypedHandler<login::LoginResult>(
      [&](const Address&, Channel*, const login::LoginResult& reply) {
        status = reply.status;
        received = true;
      }));
  auto connected = connect_login_client(Address("127.0.0.1", client.RudpAddress().Port()));
  ASSERT_TRUE(connected);
  IntrusivePtr<Channel> channel{*connected};
  InterfaceTable table;
  CapturingChannel database(dispatcher_, table, Address("127.0.0.1", 24002));
  set_legacy_dbapp_channel(&database);
  const auto request = start_login(channel->ChannelId(), "slow-auth");
  const auto after_idle = Clock::now() + login_idle_timeout() + Milliseconds(100);
  ASSERT_TRUE(drive_until([&] { return Clock::now() >= after_idle; },
                          login_idle_timeout() + Milliseconds(500)));
  ASSERT_EQ(active_login_count(), 1u);
  ASSERT_TRUE(channel->IsConnected());
  login::AuthLoginResult reply;
  reply.request_id = request;
  reply.status = login::LoginStatus::kInvalidCredentials;
  EXPECT_TRUE(handle_auth_result(&database, reply));
  EXPECT_TRUE(drive_until([&] { return received; }, Milliseconds(500)));
  EXPECT_EQ(status, login::LoginStatus::kInvalidCredentials);
  EXPECT_EQ(active_login_count(), 0u);
  EXPECT_TRUE(pending_by_username_empty());
  EXPECT_TRUE(drive_until([&] { return !channel->IsConnected(); },
                          login_idle_timeout() * 2 + Milliseconds(500)));
}

TEST_F(LoginRollbackTest, IdleClientWithoutPendingLoginStillExpires) {
  NetworkInterface client(dispatcher_);
  ASSERT_TRUE(client.StartRudpServer(Address("127.0.0.1", 0)));
  auto connected = connect_login_client(Address("127.0.0.1", client.RudpAddress().Port()));
  ASSERT_TRUE(connected);
  IntrusivePtr<Channel> channel{*connected};
  EXPECT_TRUE(drive_until([&] { return !channel->IsConnected(); },
                          login_idle_timeout() + Milliseconds(500)));
}

TEST_F(LoginRollbackTest, DisconnectDuringPendingLoginCancelsWithoutRestoringDeadChannel) {
  NetworkInterface client(dispatcher_);
  ASSERT_TRUE(client.StartRudpServer(Address("127.0.0.1", 0)));
  auto connected = connect_login_client(Address("127.0.0.1", client.RudpAddress().Port()));
  ASSERT_TRUE(connected);
  IntrusivePtr<Channel> channel{*connected};
  InterfaceTable table;
  CapturingChannel database(dispatcher_, table, Address("127.0.0.1", 24002));
  set_legacy_dbapp_channel(&database);
  const auto request = start_login(channel->ChannelId(), "disconnect-pending");
  ASSERT_EQ(active_login_count(), 1u);
  ASSERT_EQ(pending_rpc_count(), 1u);
  disconnect_login_client(*channel);
  EXPECT_EQ(active_login_count(), 0u);
  EXPECT_EQ(pending_rpc_count(), 0u);
  EXPECT_TRUE(pending_by_username_empty());
  EXPECT_FALSE(login_channel_exists(channel->ChannelId()));
  EXPECT_TRUE(channel->IsCondemned());
  login::AuthLoginResult late;
  late.request_id = request;
  EXPECT_FALSE(handle_auth_result(&database, late));
}

TEST_F(LoginRollbackTest, DedupTrackingAndMetrics) {
  register_watchers();

  add_pending_username("alice", 1);
  add_pending_username("bob", 2);

  EXPECT_FALSE(pending_by_username_empty());
  EXPECT_EQ(watcher("loginapp/pending_logins").value_or(""), "2");

  remove_pending_username("alice");
  remove_pending_username("bob");
  EXPECT_TRUE(pending_by_username_empty());
  EXPECT_EQ(watcher("loginapp/pending_logins").value_or(""), "0");
}

TEST_F(LoginRollbackTest, AuthLoginUsesLegacyDbappWhenShardTableMissing) {
  auto* legacy = test_support::FakeChannel(0x1501);
  set_legacy_dbapp_channel(legacy);

  EXPECT_EQ(resolve_auth_dbapp("alice"), legacy);
}

TEST_F(LoginRollbackTest, AuthLoginRoutesUsernameThroughShardTable) {
  register_watchers();
  const Address shard_a_addr("127.0.0.1", 31001);
  const Address shard_b_addr("127.0.0.1", 31002);
  auto* shard_a = test_support::FakeChannel(0x1502);
  auto* shard_b = test_support::FakeChannel(0x1503);
  cache_dbapp_channel(shard_a_addr, shard_a);
  cache_dbapp_channel(shard_b_addr, shard_b);

  const DatabaseID kSplit = std::numeric_limits<DatabaseID>::max() / 2;
  apply_dbapp_shards(
      3, {dbappmgr::ShardEntry{1, kSplit, 1, shard_a_addr},
          dbappmgr::ShardEntry{kSplit, std::numeric_limits<DatabaseID>::max(), 2, shard_b_addr}});

  std::string routed_to_a;
  std::string routed_to_b;
  for (int i = 0; i < 10000 && (routed_to_a.empty() || routed_to_b.empty()); ++i) {
    std::string username = std::format("user{}", i);
    Channel* target = resolve_auth_dbapp(username);
    if (target == shard_a && routed_to_a.empty()) routed_to_a = username;
    if (target == shard_b && routed_to_b.empty()) routed_to_b = username;
  }

  ASSERT_FALSE(routed_to_a.empty());
  ASSERT_FALSE(routed_to_b.empty());
  EXPECT_EQ(resolve_auth_dbapp(routed_to_a), shard_a);
  EXPECT_EQ(resolve_auth_dbapp(routed_to_b), shard_b);
  EXPECT_EQ(watcher("loginapp/dbapp_shard_table_version").value_or(""), "3");
  EXPECT_EQ(watcher("loginapp/dbapp_shard_count").value_or(""), "2");
}

TEST_F(LoginRollbackTest, DbappDeathClearsCachedShardChannel) {
  const Address shard_addr("127.0.0.1", 31003);
  cache_dbapp_channel(shard_addr, test_support::FakeChannel(0x1504));
  EXPECT_EQ(cached_dbapp_channel_count(), 1u);

  handle_dbapp_death(shard_addr);

  EXPECT_EQ(cached_dbapp_channel_count(), 0u);
}

TEST_F(LoginRollbackTest, AuthLoginRetriesAfterDbappDeathAndShardUpdate) {
  InterfaceTable channel_table;
  CapturingChannel old_shard(dispatcher_, channel_table, Address(0x7F000001u, 31004));
  CapturingChannel new_shard(dispatcher_, channel_table, Address(0x7F000001u, 31005));
  const Address old_addr(0x7F000001u, 31004);
  const Address new_addr(0x7F000001u, 31005);

  cache_dbapp_channel(old_addr, &old_shard);
  apply_dbapp_shards(
      7, {dbappmgr::ShardEntry{1, std::numeric_limits<DatabaseID>::max(), 8, old_addr}});

  const uint32_t request_id = start_login(77, "alice");

  const auto first_auths = DecodeSentMessages<login::AuthLogin>(old_shard);
  ASSERT_EQ(first_auths.size(), 1u);
  EXPECT_EQ(first_auths[0].request_id, request_id);
  EXPECT_EQ(first_auths[0].username, "alice");

  handle_dbapp_death(old_addr);
  cache_dbapp_channel(new_addr, &new_shard);
  apply_dbapp_shards(
      8, {dbappmgr::ShardEntry{1, std::numeric_limits<DatabaseID>::max(), 9, new_addr}});

  EXPECT_TRUE(drive_until([&] { return new_shard.send_count() > 0; }, Milliseconds(500)));
  const auto retried_auths = DecodeSentMessages<login::AuthLogin>(new_shard);
  ASSERT_EQ(retried_auths.size(), 1u);
  EXPECT_EQ(retried_auths[0].request_id, request_id);
  EXPECT_EQ(retried_auths[0].username, "alice");

  login::AuthLoginResult auth_reply;
  auth_reply.request_id = request_id;
  auth_reply.success = true;
  auth_reply.status = login::LoginStatus::kSuccess;
  auth_reply.dbid = 42;
  auth_reply.type_id = 7;
  EXPECT_FALSE(handle_auth_result(&old_shard, auth_reply));
  EXPECT_FALSE(handle_auth_result(nullptr, auth_reply));
  EXPECT_TRUE(handle_auth_result(&new_shard, auth_reply));

  EXPECT_TRUE(pending_by_username_empty());
  EXPECT_EQ(active_login_count(), 0u);
}

class BaseAppRollbackTest : public ::testing::Test {
 protected:
  BaseAppRollbackTest()
      : dispatcher_("baseapp_rollback"),
        internal_network_(dispatcher_),
        external_network_(dispatcher_),
        app_(dispatcher_, internal_network_, external_network_) {
    const_cast<ServerConfig&>(app_.Config()).process_instance = 111;
    app_.RegisterInternalHandlers();
  }

  void install_checkout_type() {
    auto& definitions = EntityDefRegistry::Instance();
    BinaryWriter type;
    type.WriteString("CheckoutType");
    type.Write(uint16_t{7});
    type.Write(uint8_t{0});
    type.Write(uint8_t{0});
    type.WritePackedInt(0);
    type.WritePackedInt(0);
    ASSERT_TRUE(
        definitions.RegisterType(type.Data().data(), static_cast<int32_t>(type.Data().size())));
    checkout_type_registered_ = true;
  }
  void TearDown() override {
    if (checkout_type_registered_) EntityDefRegistry::Instance().clear();
  }
  void set_space_manager(Channel* channel) { app_.cellappmgr_channel_ = channel; }
  void answer_space(Channel& channel, const cellappmgr::SpaceCreatedResult& reply) {
    app_.OnSpaceCreatedResult(channel, reply);
  }
  auto pending_spaces() const -> std::size_t { return app_.pending_space_creates_.size(); }
  void expire_spaces() {
    for (auto& [_, pending] : app_.pending_space_creates_)
      pending.started_at = Clock::now() - std::chrono::seconds(6);
    app_.CleanupExpiredPendingRequests();
  }
  void fail_spaces() { app_.FailPendingSpaceCreates(); }
  void exhaust_space_ids() { app_.next_space_request_id_ = 0; }
  auto request_space(SpaceID id, BaseApp::SpaceCreatedCallback callback) -> uint32_t {
    return app_.RequestCreateSpace(id, std::move(callback));
  }
  void load_from_db(Channel& channel, const baseapp::CreateBaseFromDB& request) {
    app_.OnCreateBaseFromDb(channel, request);
  }

  void register_watchers() { app_.RegisterWatchers(); }

  void seed_expired_prepared_login(uint32_t login_request_id, EntityID entity_id) {
    app_.prepared_login_entities_[login_request_id] = BaseApp::PreparedLoginEntity{
        entity_id, 1234, 7, Clock::now() - BaseApp::kPreparedLoginTimeout - std::chrono::seconds(1),
        "alice"};
    app_.prepared_login_requests_by_entity_[entity_id] = login_request_id;
  }

  void cleanup_expired() { app_.CleanupExpiredPendingRequests(); }

  auto query_session(std::shared_ptr<std::optional<Result<AccountSession>>> result,
                     std::optional<AccountSessionOperation> operation = {},
                     CancellationToken token = {}) -> FireAndForget {
    *result = co_await app_.QueryAccountSession(42, 7, std::move(operation), std::move(token));
  }
  void session_reply(Channel* channel, const dbapp::AccountSessionReply& reply) {
    app_.OnAccountSessionReply(channel, reply);
  }
  auto session_pending_count() const -> std::size_t { return app_.rpc_registry_.PendingCount(); }
  void disconnect_session_channel(Channel* channel) { app_.rpc_registry_.CancelByChannel(channel); }

  auto new_proxy(DatabaseID dbid = kInvalidDBID) -> Proxy* {
    app_.entity_mgr_.SetIdClient(&app_.id_client_);
    if (!ids_seeded_) {
      app_.id_client_.AddIds(1000, 1100);
      ids_seeded_ = true;
    }
    auto* entity = app_.entity_mgr_.Create(7, true, dbid);
    return entity ? app_.entity_mgr_.FindProxy(entity->EntityId()) : nullptr;
  }
  auto handoff_target(EntityID entity_id) -> std::optional<AccountSessionOwner> {
    BaseAppNativeProvider provider(app_);
    uint32_t ip = 17;
    uint16_t port = 19;
    uint64_t instance = 23;
    if (!provider.GetClientHandoffTarget(entity_id, ip, port, instance)) {
      EXPECT_EQ(ip, 0u);
      EXPECT_EQ(port, 0u);
      EXPECT_EQ(instance, 0u);
      return {};
    }
    return AccountSessionOwner{Address(ip, port), entity_id, instance};
  }
  auto prepare_auth_proxy(DatabaseID dbid = 42) -> Proxy* {
    auto* proxy = new_proxy(dbid);
    if (!proxy) return nullptr;
    auto key = SessionKey::Generate();
    EXPECT_TRUE(key.HasValue());
    if (!key) return nullptr;
    EXPECT_TRUE(app_.entity_mgr_.AssignSessionKey(proxy->EntityId(), *key));
    CheckoutInfo checkout{Address("127.0.0.1", 25001), 7, proxy->EntityId(), 9, 111};
    proxy->SetCheckoutOwner(checkout);
    const auto request_id = proxy->EntityId() + 100;
    app_.prepared_login_entities_[request_id] = BaseApp::PreparedLoginEntity{
        proxy->EntityId(), dbid, 7, Clock::now(), std::format("session-test-{}", dbid), checkout};
    app_.prepared_login_requests_by_entity_[proxy->EntityId()] = request_id;
    return proxy;
  }
  void authenticate(Channel& channel, const SessionKey& key, bool supports_handoff = false,
                    bool supports_recovery = false) {
    baseapp::Authenticate request;
    request.session_key = key;
    request.supports_handoff = supports_handoff;
    request.supports_recovery = supports_recovery;
    app_.OnClientAuthenticate(channel, request);
  }
  auto handoff_request(Proxy& target, const SessionKey& credential)
      -> baseapp::PrepareClientHandoff {
    baseapp::PrepareClientHandoff request;
    request.request_id = (uint64_t{1} << 40) + 5;
    request.requester_instance = 222;
    request.account_dbid = 42;
    request.account_type_id = 7;
    request.username = "handoff-test";
    request.expected.version = 5;
    request.expected.phase = AccountSessionPhase::kPreparing;
    request.expected.owner = {Address("127.0.0.1", 25002), 600, 222};
    request.expected.target = {Address("127.0.0.1", 25001), target.EntityId(), 111};
    request.expected.handoff_id[0] = std::byte{1};
    auto hash = Sha256(std::as_bytes(std::span{credential.bytes}));
    EXPECT_TRUE(hash.HasValue());
    if (hash) request.expected.credential_hash = *hash;
    std::ranges::copy(std::as_bytes(EntityDefRegistry::Instance().Digest()),
                      request.entity_def_digest.begin());
    register_base_peer(request.expected.owner.base_addr, 222);
    register_base_peer(request.expected.target.base_addr, 111);
    return request;
  }
  void prepare_handoff(Channel& channel, const baseapp::PrepareClientHandoff& request) {
    app_.OnPrepareClientHandoff(IntrusivePtr<Channel>{&channel}, request);
  }
  auto dispatch_internal(Channel& channel, MessageID id, std::span<const std::byte> bytes) -> bool {
    BinaryReader reader(bytes);
    return internal_network_.InterfaceTable()
        .Dispatch(channel.RemoteAddress(), &channel, id, reader)
        .HasValue();
  }
  auto incoming_handoff_count() const -> std::size_t {
    return app_.incoming_client_handoffs_.size();
  }
  void fill_handoff_queue() {
    const auto incoming = app_.incoming_client_handoffs_.begin()->second;
    for (EntityID id = 2000; app_.incoming_client_handoffs_.size() < 1024; ++id)
      app_.incoming_client_handoffs_.emplace(id, incoming);
  }
  void expire_handoffs() {
    for (auto& [_, incoming] : app_.incoming_client_handoffs_)
      incoming->checked_at = Clock::now() - std::chrono::seconds(21);
    app_.SweepIncomingClientHandoffs();
  }
  auto apply_latest(CapturingChannel& database, const AccountSession& current) -> AccountSession {
    const auto requests = DecodeSentMessages<dbapp::AccountSessionRequest>(database);
    EXPECT_FALSE(requests.empty());
    if (requests.empty() || !requests.back().operation) {
      ADD_FAILURE() << "expected session mutation";
      return current;
    }
    auto next = ApplySessionOperation(current, *requests.back().operation, 1000);
    EXPECT_TRUE(next.HasValue());
    return next ? *next : current;
  }
  void answer_session(CapturingChannel& channel, const AccountSession& record,
                      ErrorCode status = ErrorCode::kNone) {
    const auto requests = DecodeSentMessages<dbapp::AccountSessionRequest>(channel);
    ASSERT_FALSE(requests.empty());
    const auto& request = requests.back();
    dbapp::AccountSessionReply reply;
    reply.request_id = request.request_id;
    reply.requester_instance = request.process_instance;
    reply.account_dbid = request.account_dbid;
    reply.account_type_id = request.account_type_id;
    if (request.operation) reply.operation_id = request.operation->id;
    reply.session = record;
    reply.status = status;
    if (status != ErrorCode::kNone) reply.error = "injected session failure";
    app_.OnAccountSessionReply(&channel, reply);
  }
  auto active_session(Proxy& proxy, uint64_t version = 1) -> AccountSession {
    AccountSession record;
    record.version = version;
    record.phase = AccountSessionPhase::kActive;
    const auto& checkout = *proxy.CheckoutOwner();
    record.owner = {checkout.base_addr, proxy.EntityId(), checkout.process_instance};
    return record;
  }
  auto prepare_resume(login::PrepareResume request,
                      std::shared_ptr<std::optional<Result<login::PrepareResumeResult>>> result)
      -> FireAndForget {
    *result = co_await app_.PrepareSessionRecovery(std::move(request));
  }
  void logout(Channel& channel, EntityID entity_id, uint64_t request_id) {
    app_.OnClientLogout(IntrusivePtr<Channel>{&channel},
                        baseapp::ClientLogout{request_id, entity_id});
  }
  void refresh_recovery_after_save(EntityID entity_id, DatabaseID dbid) {
    ASSERT_TRUE(app_.entity_mgr_.AssignDbid(entity_id, dbid));
    app_.RefreshPersistedRecoveryCredential(entity_id);
  }
  auto recovery_target(EntityID entity_id) const -> DatabaseID {
    return app_.client_sessions_.at(entity_id)->record.recovery.entity_dbid;
  }
  auto retained_client_channel(const Address& address) -> Channel* {
    auto channel = external_network_.ConnectRudp(address);
    return channel ? static_cast<Channel*>(*channel) : nullptr;
  }
  void prepare_crash_recovery() {
    install_checkout_type();
    app_.entity_mgr_.SetIdClient(&app_.id_client_);
    app_.id_client_.AddIds(1000, 1100);
    app_.app_id_ = 7;
    register_base_peer(Address("127.0.0.1", 25001), 111);
    app_.baseapp_peers_.at(Address("127.0.0.1", 25001)).external_addr = Address("127.0.0.1", 26001);
  }
  void answer_recovery_checkout(CapturingChannel& database) {
    const auto requests = DecodeSentMessages<dbapp::CheckoutEntity>(database);
    ASSERT_FALSE(requests.empty());
    const auto& request = requests.back();
    dbapp::CheckoutEntityAck reply;
    reply.request_id = request.request_id;
    reply.operation_id = request.operation_id;
    reply.dbid = request.dbid;
    reply.checkout_version = 19;
    reply.holder_addr = Address("127.0.0.1", 25001);
    reply.holder_app_id = request.owner_app_id;
    reply.holder_entity_id = request.entity_id;
    reply.holder_process_instance = request.owner_process_instance;
    reply.blob = DatabaseBlob({std::byte{0x11}}, {std::byte{0x22}});
    handle_checkout_ack(database.RemoteAddress(), reply);
  }
  auto resume_request(const AccountSession& record) -> login::PrepareResume {
    register_base_peer(record.owner.base_addr, record.owner.process_instance);
    app_.baseapp_peers_.at(record.owner.base_addr).external_addr = Address("127.0.0.1", 26001);
    login::PrepareResume request;
    request.request_id = 700;
    request.requester_instance = 333;
    request.account_dbid = 42;
    request.account_type_id = 7;
    request.proof = record.recovery.credential_hash;
    request.session_key = *SessionKey::Generate();
    std::ranges::copy(EntityDefRegistry::Instance().Digest(), request.entity_def_digest.begin());
    register_base_peer(record.owner.base_addr, record.owner.process_instance);
    app_.baseapp_peers_.at(record.owner.base_addr).external_addr = Address("127.0.0.1", 26001);
    return request;
  }
  void retain_client_channel(const Address& address) {
    ASSERT_TRUE(external_network_.ConnectRudp(address).HasValue());
  }
  auto move_session(EntityID source, EntityID destination,
                    std::shared_ptr<std::optional<Result<void>>> result) -> FireAndForget {
    *result = co_await app_.MoveClientSession(source, destination);
  }
  auto move_remote_session(EntityID source, AccountSessionOwner target,
                           std::shared_ptr<std::optional<Result<void>>> result) -> FireAndForget {
    *result = co_await app_.MoveRemoteClientSession(source, target);
  }
  auto recover_remote_session(EntityID source, std::shared_ptr<std::optional<Result<void>>> result)
      -> FireAndForget {
    auto session = app_.client_sessions_.at(source);
    session->frozen = true;
    session->remote_handoff = true;
    *result = co_await app_.RecoverClientHandoff(session, {}, Error{ErrorCode::kReceiverGone});
  }
  auto release_session(EntityID entity_id, std::shared_ptr<std::optional<Result<void>>> result)
      -> FireAndForget {
    *result = co_await app_.ReleaseClientSession(entity_id);
  }
  auto session_frozen(EntityID entity_id) const -> bool {
    return app_.HasFrozenClientSession(entity_id);
  }
  auto transfer_target_reserved(EntityID entity_id) const -> bool {
    return app_.client_transfer_targets_.contains(entity_id);
  }
  auto finalize_proxy(EntityID entity_id) -> bool { return app_.FinalizeForceLogoff(entity_id); }
  auto rollback_auth_proxy(Proxy& proxy) -> bool {
    return app_.RollbackPreparedLoginEntity(proxy.EntityId() + 100);
  }
  void register_base_peer(const Address& address, uint64_t instance) {
    machined::BirthNotification birth;
    birth.process_type = ProcessType::kBaseApp;
    birth.internal_addr = address;
    birth.process_instance = instance;
    app_.OnBaseAppBirth(birth);
  }
  void retire_base_peer(const Address& address, uint64_t instance) {
    machined::DeathNotification death;
    death.internal_addr = address;
    death.process_instance = instance;
    app_.OnBaseAppDeath(death);
  }
  auto base_peer_instance(const Address& address) const -> uint64_t {
    const auto peer = app_.baseapp_peers_.find(address);
    return peer == app_.baseapp_peers_.end() ? 0 : peer->second.process_instance;
  }
  void revoke_session(Channel& channel, const baseapp::RevokeAccountSession& request) {
    app_.OnRevokeAccountSession(IntrusivePtr<Channel>{&channel}, request);
  }
  auto revocation_count() const -> std::size_t { return app_.session_revocations_; }
  void set_revocations_full(bool full) { app_.session_revocations_ = full ? 1024 : 0; }
  void prepare_login(Channel& channel, uint32_t request_id = 17) {
    login::PrepareLogin request;
    request.request_id = request_id;
    request.dbid = 42;
    request.type_id = 7;
    request.username = "preflight-test";
    const auto digest = EntityDefRegistry::Instance().Digest();
    std::copy(digest.begin(), digest.end(), request.entity_def_digest.begin());
    app_.OnPrepareLogin(channel, request);
  }
  void cancel_login(Channel& channel, uint32_t request_id = 17) {
    login::CancelPrepareLogin request;
    request.request_id = request_id;
    request.dbid = 42;
    app_.OnCancelPrepareLogin(channel, request);
  }
  auto login_session_check_count() const -> std::size_t {
    return app_.login_session_checks_.size();
  }
  void check_remote_revocation_roundtrip() {
    ASSERT_TRUE(internal_network_.StartRudpServer(Address("127.0.0.1", 0)));
    NetworkInterface remote_network(dispatcher_);
    NetworkInterface remote_external(dispatcher_);
    ASSERT_TRUE(remote_network.StartRudpServer(Address("127.0.0.1", 0)));
    BaseApp remote(dispatcher_, remote_network, remote_external);
    const_cast<ServerConfig&>(remote.Config()).process_instance = 222;
    remote.RegisterInternalHandlers();
    InterfaceTable table;
    auto database =
        make_intrusive<CapturingChannel>(dispatcher_, table, Address("127.0.0.1", 24002));
    auto remote_database =
        make_intrusive<CapturingChannel>(dispatcher_, table, Address("127.0.0.1", 24003));
    auto login = make_intrusive<CapturingChannel>(dispatcher_, table, Address("127.0.0.1", 28001));
    set_legacy_dbapp_channel(database.get());
    remote.dbapp_channel_ = remote_database.get();
    const auto source_addr = internal_network_.RudpAddress();
    const auto target_addr = remote_network.RudpAddress();
    register_base_peer(source_addr, 111);
    register_base_peer(target_addr, 222);
    for (const auto& [_, peer] : app_.baseapp_peers_) remote.OnBaseAppBirth(peer);
    prepare_login(*login);
    AccountSession grant;
    grant.version = 9;
    grant.phase = AccountSessionPhase::kActive;
    grant.owner = {target_addr, 600, 222};
    answer_session(*database, grant);
    const auto deadline = Clock::now() + std::chrono::seconds(2);
    while (remote_database->send_count() == 0 && Clock::now() < deadline) dispatcher_.ProcessOnce();
    auto reads = DecodeSentMessages<dbapp::AccountSessionRequest>(*remote_database);
    ASSERT_EQ(reads.size(), 1u);
    EXPECT_EQ(login_session_check_count(), 1u);
    EXPECT_TRUE(DecodeSentMessages<dbapp::CheckoutEntity>(*database).empty());
    auto answer_remote = [&](const AccountSession& record) {
      const auto requests = DecodeSentMessages<dbapp::AccountSessionRequest>(*remote_database);
      const auto& request = requests.back();
      dbapp::AccountSessionReply reply;
      reply.request_id = request.request_id;
      reply.requester_instance = 222;
      reply.account_dbid = request.account_dbid;
      reply.account_type_id = request.account_type_id;
      if (request.operation) reply.operation_id = request.operation->id;
      reply.session = record;
      remote.OnAccountSessionReply(remote_database.get(), reply);
    };
    answer_remote(grant);
    auto writes = DecodeSentMessages<dbapp::AccountSessionRequest>(*remote_database);
    ASSERT_EQ(writes.size(), 2u);
    ASSERT_TRUE(writes.back().operation.has_value());
    EXPECT_EQ(writes.back().operation->expected_version, grant.version);
    EXPECT_EQ(writes.back().operation->actor, grant.owner);
    EXPECT_TRUE(DecodeSentMessages<dbapp::CheckoutEntity>(*database).empty());
    AccountSession offline;
    offline.version = 10;
    answer_remote(offline);
    const auto complete_deadline = Clock::now() + std::chrono::seconds(2);
    while (login_session_check_count() != 0 && Clock::now() < complete_deadline)
      dispatcher_.ProcessOnce();
    EXPECT_EQ(login_session_check_count(), 0u);
    EXPECT_EQ(DecodeSentMessages<dbapp::CheckoutEntity>(*database).size(), 1u);
    EXPECT_EQ(remote.session_revocations_, 0u);
  }
  auto wait_handoff_reply(
      Channel* channel, baseapp::PrepareClientHandoff request,
      std::shared_ptr<std::optional<Result<baseapp::PrepareClientHandoffReply>>> result)
      -> FireAndForget {
    *result = co_await RpcCall<baseapp::PrepareClientHandoffReply>(
        app_.rpc_registry_, *channel, request, std::chrono::seconds(2));
  }
  void check_handoff_roundtrip() {
    ASSERT_TRUE(internal_network_.StartRudpServer(Address("127.0.0.1", 0)));
    NetworkInterface remote_network(dispatcher_);
    NetworkInterface remote_external(dispatcher_);
    ASSERT_TRUE(remote_network.StartRudpServer(Address("127.0.0.1", 0)));
    BaseApp remote(dispatcher_, remote_network, remote_external);
    const_cast<ServerConfig&>(remote.Config()).process_instance = 222;
    remote.RegisterInternalHandlers();
    remote.entity_mgr_.SetIdClient(&remote.id_client_);
    remote.id_client_.AddIds(2000, 2100);
    ASSERT_NE(remote.entity_mgr_.Create(7, true), nullptr);
    auto* target = remote.entity_mgr_.FindProxy(2000);
    ASSERT_NE(target, nullptr);
    auto key = SessionKey::Generate();
    ASSERT_TRUE(key.HasValue());
    auto request = handoff_request(*target, *key);
    const auto source_addr = internal_network_.RudpAddress();
    const auto target_addr = remote_network.RudpAddress();
    request.requester_instance = 111;
    request.expected.owner = {source_addr, 600, 111};
    request.expected.target = {target_addr, 2000, 222};
    register_base_peer(source_addr, 111);
    register_base_peer(target_addr, 222);
    for (const auto& [_, peer] : app_.baseapp_peers_) remote.OnBaseAppBirth(peer);
    InterfaceTable table;
    auto database =
        make_intrusive<CapturingChannel>(dispatcher_, table, Address("127.0.0.1", 24002));
    remote.dbapp_channel_ = database.get();
    auto channel = internal_network_.ConnectRudpNocwnd(target_addr);
    ASSERT_TRUE(channel.HasValue());
    auto result = std::make_shared<std::optional<Result<baseapp::PrepareClientHandoffReply>>>();
    wait_handoff_reply(*channel, request, result);
    const auto deadline = Clock::now() + std::chrono::seconds(2);
    while (database->send_count() == 0 && Clock::now() < deadline) dispatcher_.ProcessOnce();
    ASSERT_EQ(database->send_count(), 1u);
    EXPECT_FALSE(result->has_value());
    baseapp::PrepareClientHandoffReply wrong;
    wrong.request_id = request.request_id;
    wrong.requester_instance = 999;
    wrong.account_dbid = request.account_dbid;
    wrong.account_type_id = request.account_type_id;
    BinaryWriter bytes;
    wrong.Serialize(bytes);
    EXPECT_TRUE(dispatch_internal(**channel, wrong.Descriptor().id, bytes.Data()));
    EXPECT_FALSE(result->has_value());
    auto answer = [&](const AccountSession& record) {
      const auto pending = DecodeSentMessages<dbapp::AccountSessionRequest>(*database).back();
      dbapp::AccountSessionReply reply;
      reply.request_id = pending.request_id;
      reply.requester_instance = 222;
      reply.account_dbid = pending.account_dbid;
      reply.account_type_id = pending.account_type_id;
      if (pending.operation) reply.operation_id = pending.operation->id;
      reply.session = record;
      remote.OnAccountSessionReply(database.get(), reply);
    };
    answer(request.expected);
    EXPECT_FALSE(result->has_value());
    auto accepted = apply_latest(*database, request.expected);
    answer(accepted);
    const auto complete_deadline = Clock::now() + std::chrono::seconds(2);
    while (!result->has_value() && Clock::now() < complete_deadline) dispatcher_.ProcessOnce();
    ASSERT_TRUE(result->has_value());
    ASSERT_TRUE((*result)->HasValue());
    EXPECT_EQ((**result)->status, ErrorCode::kNone);
    EXPECT_EQ((**result)->session, accepted);
    EXPECT_FALSE(target->HasClient());
    EXPECT_EQ(app_.rpc_registry_.PendingCount(), 0u);
    EXPECT_EQ(remote.rpc_registry_.PendingCount(), 0u);
  }
  void check_outgoing_handoff(bool occupied = false, bool unknown = false, bool unsupported = false,
                              bool recovery = false) {
    ASSERT_TRUE(internal_network_.StartRudpServer(Address("127.0.0.1", 0)));
    ASSERT_TRUE(external_network_.StartRudpServer(Address("127.0.0.1", 0)));
    NetworkInterface remote_network(dispatcher_);
    NetworkInterface remote_external(dispatcher_);
    NetworkInterface client_network(dispatcher_);
    ASSERT_TRUE(remote_network.StartRudpServer(Address("127.0.0.1", 0)));
    ASSERT_TRUE(remote_external.StartRudpServer(Address("127.0.0.1", 0)));
    ASSERT_TRUE(client_network.StartRudpServer(Address("127.0.0.1", 0)));
    BaseApp remote(dispatcher_, remote_network, remote_external);
    const_cast<ServerConfig&>(remote.Config()).process_instance = 222;
    remote.RegisterInternalHandlers();
    remote.entity_mgr_.SetIdClient(&remote.id_client_);
    remote.id_client_.AddIds(2000, 2100);
    ASSERT_NE(remote.entity_mgr_.Create(7, true), nullptr);
    auto* target = remote.entity_mgr_.FindProxy(2000);
    ASSERT_NE(target, nullptr);
    if (occupied) target->BindClient(Address("127.0.0.1", 29001));
    const auto source_addr = internal_network_.RudpAddress();
    const auto target_addr = remote_network.RudpAddress();
    register_base_peer(source_addr, 111);
    register_base_peer(target_addr, 222);
    app_.baseapp_peers_.at(target_addr).external_addr = remote_external.RudpAddress();
    for (const auto& [_, peer] : app_.baseapp_peers_) remote.OnBaseAppBirth(peer);
    InterfaceTable table;
    auto database =
        make_intrusive<CapturingChannel>(dispatcher_, table, Address("127.0.0.1", 24002));
    auto remote_database =
        make_intrusive<CapturingChannel>(dispatcher_, table, Address("127.0.0.1", 24003));
    auto client =
        make_intrusive<CapturingChannel>(dispatcher_, table, client_network.RudpAddress());
    set_legacy_dbapp_channel(database.get());
    remote.dbapp_channel_ = remote_database.get();
    auto* source = prepare_auth_proxy();
    ASSERT_NE(source, nullptr);
    auto checkout = *source->CheckoutOwner();
    checkout.base_addr = source_addr;
    source->SetCheckoutOwner(checkout);
    app_.prepared_login_entities_.at(source->EntityId() + 100).checkout_owner = checkout;
    retain_client_channel(client_network.RudpAddress());
    authenticate(*client, source->GetSessionKey(), !unsupported);
    answer_session(*database, {});
    const auto active = active_session(*source);
    answer_session(*database, active);
    std::optional<baseapp::ClientHandoff> redirect;
    ASSERT_TRUE(client_network.InterfaceTable().RegisterTypedHandler<baseapp::ClientHandoff>(
        [&](const Address&, Channel*, const baseapp::ClientHandoff& request) {
          redirect = request;
        }));
    auto result = std::make_shared<std::optional<Result<void>>>();
    move_remote_session(source->EntityId(), {target_addr, target->EntityId(), 222}, result);
    if (unsupported) {
      ASSERT_TRUE(result->has_value());
      ASSERT_FALSE((*result)->HasValue());
      EXPECT_EQ((*result)->Error().Code(), ErrorCode::kNotSupported);
      EXPECT_EQ(database->send_count(), 2u);
      EXPECT_TRUE(source->HasClient());
      EXPECT_FALSE(session_frozen(source->EntityId()));
      return;
    }
    EXPECT_TRUE(source->HasClient());
    EXPECT_TRUE(session_frozen(source->EntityId()));
    const auto prepared = apply_latest(*database, active);
    answer_session(*database, prepared);
    auto pump = [&](auto predicate) {
      const auto deadline = Clock::now() + std::chrono::seconds(2);
      while (!predicate() && Clock::now() < deadline) dispatcher_.ProcessOnce();
      return predicate();
    };
    if (occupied) {
      ASSERT_TRUE(pump([&] { return database->send_count() == 4; }));
      answer_session(*database, prepared);
      const auto aborted = apply_latest(*database, prepared);
      answer_session(*database, aborted);
      ASSERT_TRUE(result->has_value());
      EXPECT_FALSE((*result)->HasValue());
      EXPECT_FALSE(session_frozen(source->EntityId()));
      EXPECT_TRUE(source->HasClient());
      EXPECT_FALSE(redirect.has_value());
      EXPECT_EQ(remote_database->send_count(), 0u);
      return;
    }
    ASSERT_TRUE(pump([&] { return remote_database->send_count() != 0; }));
    auto answer_remote = [&](const AccountSession& record, ErrorCode status = ErrorCode::kNone) {
      const auto pending =
          DecodeSentMessages<dbapp::AccountSessionRequest>(*remote_database).back();
      dbapp::AccountSessionReply reply;
      reply.request_id = pending.request_id;
      reply.requester_instance = 222;
      reply.account_dbid = pending.account_dbid;
      reply.account_type_id = pending.account_type_id;
      if (pending.operation) reply.operation_id = pending.operation->id;
      reply.session = record;
      reply.status = status;
      remote.OnAccountSessionReply(remote_database.get(), reply);
    };
    answer_remote(prepared);
    const auto accepted = apply_latest(*remote_database, prepared);
    answer_remote(accepted);
    ASSERT_TRUE(pump([&] { return database->send_count() == 4; }));
    const auto awaiting = apply_latest(*database, accepted);
    answer_session(*database, awaiting);
    ASSERT_TRUE(pump([&] {
      return redirect.has_value() && remote.incoming_client_handoffs_.at(2000)->waiter.has_value();
    }));
    EXPECT_EQ(redirect->target.base_addr, remote_external.RudpAddress());
    EXPECT_EQ(redirect->target.process_instance, 222u);
    EXPECT_TRUE(source->HasClient());
    EXPECT_FALSE(target->HasClient());
    baseapp::Authenticate authentication{redirect->credential};
    authentication.supports_recovery = recovery;
    remote.OnClientAuthenticate(*client, authentication);
    answer_remote(awaiting);
    const auto completed = apply_latest(*remote_database, awaiting);
    if (unknown) {
      answer_remote({}, ErrorCode::kCommitUnknown);
      app_.rpc_registry_.CancelByChannel(internal_network_.FindChannel(target_addr));
      answer_session(*database, completed);
      ASSERT_TRUE(result->has_value());
      EXPECT_FALSE((*result)->HasValue());
      EXPECT_EQ((*result)->Error().Code(), ErrorCode::kCommitUnknown);
      EXPECT_TRUE(source->HasClient());
      EXPECT_TRUE(session_frozen(source->EntityId()));
      EXPECT_FALSE(target->HasClient());
      return;
    }
    answer_remote(completed);
    if (recovery) {
      EXPECT_TRUE(source->HasClient());
      EXPECT_FALSE(target->HasClient());
      const auto issued = apply_latest(*remote_database, completed);
      answer_remote(issued);
    }
    EXPECT_TRUE(source->HasClient());
    ASSERT_TRUE(pump([&] { return result->has_value(); }));
    EXPECT_TRUE((*result)->HasValue());
    EXPECT_FALSE(source->HasClient());
    EXPECT_TRUE(source->GetSessionKey().IsZero());
    EXPECT_TRUE(target->HasClient());
    EXPECT_EQ(source->CheckoutOwner(), checkout);
    EXPECT_EQ(source->Dbid(), 42);
    EXPECT_EQ(target->Dbid(), kInvalidDBID);
    EXPECT_EQ(session_pending_count(), 0u);
    EXPECT_EQ(remote.rpc_registry_.PendingCount(), 0u);
  }

  auto has_prepared(uint32_t login_request_id) const -> bool {
    return app_.prepared_login_entities_.contains(login_request_id);
  }

  auto has_prepared_entity(EntityID entity_id) const -> bool {
    return app_.prepared_login_requests_by_entity_.contains(entity_id);
  }

  auto prepared_timeout_total() const -> uint64_t { return app_.prepared_login_timeout_total_; }
  auto watcher(std::string_view path) -> std::optional<std::string> {
    return app_.GetWatcherRegistry().Get(path);
  }
  void set_legacy_dbapp_channel(Channel* ch) { app_.dbapp_channel_ = ch; }
  void cache_dbapp_channel(const Address& addr, Channel* ch) { app_.dbapp_channels_[addr] = ch; }
  void handle_dbapp_death(const Address& addr) {
    machined::DeathNotification death;
    death.internal_addr = addr;
    death.reason = 1;
    app_.OnDbAppDeath(death);
  }
  void apply_dbapp_shards(uint32_t version, std::vector<dbappmgr::ShardEntry> entries) {
    app_.ApplyDbAppShardTable(version, std::move(entries));
  }
  auto resolve_dbapp(DatabaseID dbid) -> Channel* { return app_.ResolveDbAppChannel(dbid); }
  auto dbapp_shard_version() const -> uint32_t { return app_.dbapp_shard_table_version_; }
  auto dbapp_shard_count() const -> std::size_t { return app_.dbapp_shard_table_.size(); }
  auto submit_prepare_login(DatabaseID dbid, uint16_t type_id = 7) -> uint32_t {
    BaseApp::PendingLogin pending;
    pending.login_request_id = 7001;
    pending.loginapp_addr = Address(0x7F000001u, 25001);
    pending.type_id = type_id;
    pending.dbid = dbid;
    pending.created_at = Clock::now();
    const uint32_t request_id = app_.next_prepare_request_id_;
    app_.SubmitPrepareLogin(std::move(pending));
    return request_id;
  }
  void set_request_counter(uint32_t value) { app_.next_prepare_request_id_ = value; }
  void set_app_id(uint32_t value) { app_.app_id_ = value; }
  void handle_checkout_ack(const Address& src, const dbapp::CheckoutEntityAck& ack) {
    BinaryWriter writer;
    ack.Serialize(writer);
    BinaryReader reader(writer.Data());
    ASSERT_TRUE(internal_network_.InterfaceTable().Dispatch(
        src, nullptr, dbapp::CheckoutEntityAck::Descriptor().id, reader));
  }
  void handle_abort_result(const Address& src, const dbapp::AbortCheckoutAck& ack) {
    BinaryWriter writer;
    ack.Serialize(writer);
    BinaryReader reader(writer.Data());
    ASSERT_TRUE(internal_network_.InterfaceTable().Dispatch(
        src, nullptr, dbapp::AbortCheckoutAck::Descriptor().id, reader));
  }
  void release_checkout(DatabaseID dbid, const CheckoutInfo& owner) {
    app_.ReleaseCheckout(dbid, 9, owner);
  }
  void checkin_ack(const Address& src, const dbapp::CheckinEntityAck& ack) {
    app_.OnCheckinEntityAck(src, ack);
  }
  auto pending_checkins() const -> std::size_t { return app_.pending_checkins_.size(); }
  void retry_checkin_now(uint32_t request) {
    app_.pending_checkins_.at(request).next_retry = TimePoint{};
    app_.RetryCheckins();
  }
  void retry_checkout_now(uint32_t request_id) {
    app_.pending_logins_.at(request_id).next_checkout_retry_at = TimePoint{};
    app_.RetryDisconnectedDbappLoginCheckouts();
  }
  void retry_cancel_now(uint32_t request_id) {
    app_.canceled_checkouts_.at(request_id).next_retry_at = TimePoint{};
    app_.RetryCanceledCheckouts();
  }
  auto checkout_cancel_stalled(uint32_t request_id) const -> bool {
    return app_.canceled_checkouts_.at(request_id).stalled;
  }
  auto native_checkout_contains(uint32_t request_id) const -> bool {
    return app_.pending_entity_checkouts_.contains(request_id);
  }
  void retry_native_now(uint32_t request_id) {
    app_.pending_entity_checkouts_.at(request_id).next_retry_at = TimePoint{};
    app_.RetryEntityCheckouts();
  }
  auto pending_login_contains(uint32_t request_id) const -> bool {
    return app_.pending_logins_.contains(request_id);
  }
  auto pending_login_retry_pending(uint32_t request_id) const -> bool {
    auto it = app_.pending_logins_.find(request_id);
    return it != app_.pending_logins_.end() && it->second.checkout_retry_pending;
  }
  auto active_login_contains(DatabaseID dbid) const -> bool {
    return app_.active_login_dbids_.contains(dbid);
  }
  auto pending_write_to_db_count() const -> std::size_t { return app_.pending_write_to_db_.size(); }
  auto pending_write_to_db_contains(uint32_t request_id) const -> bool {
    return app_.pending_write_to_db_.contains(request_id);
  }
  auto pending_logoff_write_contains(uint32_t request_id) const -> bool {
    return app_.pending_logoff_writes_.contains(request_id);
  }
  auto pending_logoff_write_retry_pending(uint32_t request_id) const -> bool {
    auto it = app_.pending_logoff_writes_.find(request_id);
    return it != app_.pending_logoff_writes_.end() && it->second.retry_pending;
  }
  auto logoff_in_flight_contains(EntityID entity_id) const -> bool {
    return app_.logoff_entities_in_flight_.contains(entity_id);
  }
  void handle_write_ack(const dbapp::WriteEntityAck& ack) {
    BinaryWriter writer;
    ack.Serialize(writer);
    BinaryReader reader(writer.Data());
    ASSERT_TRUE(internal_network_.InterfaceTable()
                    .Dispatch(Address{}, nullptr, dbapp::WriteEntityAck::Descriptor().id, reader)
                    .HasValue());
  }

  void seed_canceled_checkout(uint32_t request_id) {
    BaseApp::PendingDbCheckout pending;
    pending.request.request_id = request_id;
    pending.request.operation_id[0] = std::byte{1};
    pending.request.dbid = 42;
    pending.request.type_id = 7;
    pending.resolved_dbid = 42;
    pending.created_at = Clock::now();
    app_.canceled_checkouts_[request_id] = std::move(pending);
    app_.active_login_dbids_.insert(42);
  }
  auto canceled_checkout_contains(uint32_t request_id) const -> bool {
    return app_.canceled_checkouts_.contains(request_id);
  }
  void handle_abort_ack(uint32_t request_id, bool success) {
    dbapp::AbortCheckoutAck ack;
    ack.operation_id[0] = std::byte{1};
    ack.request_id = request_id;
    ack.success = success;
    BinaryWriter writer;
    ack.Serialize(writer);
    BinaryReader reader(writer.Data());
    ASSERT_TRUE(internal_network_.InterfaceTable().Dispatch(
        Address{}, nullptr, dbapp::AbortCheckoutAck::Descriptor().id, reader));
  }

  auto create_cell_bound_entity(SpaceID space_id, const Address& cell_addr,
                                bool has_cell_backup = true) -> BaseEntity* {
    app_.entity_mgr_.SetIdClient(&app_.id_client_);
    if (!ids_seeded_) {
      app_.id_client_.AddIds(1000, 1100);
      ids_seeded_ = true;
    }
    auto* ent = app_.entity_mgr_.Create(/*type_id=*/7, /*has_client=*/false);
    if (ent == nullptr) return nullptr;
    ent->SetSpaceId(space_id);
    ent->SetCell(cell_addr);
    if (has_cell_backup) ent->SetCellBackupData({std::byte{0x01}});
    return ent;
  }
  auto create_db_entity(DatabaseID dbid, uint16_t type_id = 7) -> BaseEntity* {
    app_.entity_mgr_.SetIdClient(&app_.id_client_);
    if (!ids_seeded_) {
      app_.id_client_.AddIds(1000, 1100);
      ids_seeded_ = true;
    }
    return app_.entity_mgr_.Create(type_id, false, dbid);
  }
  auto find_db_entity(DatabaseID dbid) -> BaseEntity* { return app_.entity_mgr_.FindByDbid(dbid); }
  void write_to_db(EntityID entity_id, std::span<const std::byte> data) {
    app_.DoWriteToDb(entity_id, data.data(), static_cast<int32_t>(data.size()));
  }
  auto write_to_db_async(EntityID entity_id, uint64_t request_id) -> RpcSendResult {
    return app_.DoWriteToDb(entity_id, nullptr, 0, request_id);
  }
  auto persistence_wait(uint64_t request_id, PendingRpcRegistry::ReplyCallback callback)
      -> PendingRpcRegistry::PendingHandle {
    return app_.rpc_registry_.RegisterPending(
        msg_id::Id(msg_id::Common::kEntityRpcReply), request_id, std::move(callback), [](Error) {},
        std::chrono::seconds(10));
  }
  void cancel_persistence_wait(PendingRpcRegistry::PendingHandle handle) {
    app_.rpc_registry_.Cancel(handle);
  }
  void expire_write(uint32_t request_id) {
    app_.pending_write_to_db_.at(request_id).created_at =
        Clock::now() - BaseApp::kPendingTimeout - std::chrono::seconds(1);
  }
  void retry_writes() {
    for (auto& [id, pending] : app_.pending_write_to_db_) pending.next_retry_at = TimePoint{};
    for (auto& [id, pending] : app_.pending_logoff_writes_) pending.next_retry_at = TimePoint{};
    app_.RetryDisconnectedDbappWriteRequests();
    app_.RetryDisconnectedDbappLogoffWrites();
  }
  void start_disconnect_logoff(EntityID entity_id) { app_.StartDisconnectLogoff(entity_id); }
  void begin_shutdown() { app_.Shutdown(); }
  void shutdown_tick() { app_.OnShutdownTick(); }
  auto shutdown_saves() const -> std::size_t { return app_.shutdown_saves_.size(); }
  auto shutdown_failed() const -> bool { return ServerAppShutdownTestAccess::Failed(app_); }
  void fail_deferred_login(EntityID entity_id, DatabaseID dbid, const CheckoutInfo& owner) {
    BaseApp::DeferredLoginCheckout entry;
    entry.dbid = dbid;
    entry.type_id = 7;
    entry.pending.checkout_owner = owner;
    app_.deferred_login_checkouts_[entity_id].push_back(std::move(entry));
    app_.FailDeferredPrepareLogins(entity_id, "injected failure");
  }

  auto death_notifications_total() const -> uint64_t {
    return app_.cellapp_death_notifications_total_;
  }
  auto death_restore_scheduled_total() const -> uint64_t {
    return app_.cellapp_death_restore_scheduled_total_;
  }
  auto death_restore_payload_scheduled_total() const -> uint64_t {
    return app_.cellapp_death_restore_payload_scheduled_total_;
  }
  auto death_restore_ghost_backup_scheduled_total() const -> uint64_t {
    return app_.cellapp_death_restore_ghost_backup_scheduled_total_;
  }
  auto death_restored_total() const -> uint64_t { return app_.cellapp_death_restored_total_; }
  auto death_lost_total() const -> uint64_t { return app_.cellapp_death_lost_total_; }
  auto death_restore_timeouts_total() const -> uint64_t {
    return app_.cellapp_death_restore_timeouts_total_;
  }
  auto death_restore_last_elapsed_ms() const -> uint64_t {
    return app_.cellapp_death_restore_last_elapsed_ms_;
  }
  auto death_restore_max_elapsed_ms() const -> uint64_t {
    return app_.cellapp_death_restore_max_elapsed_ms_;
  }
  auto pending_death_restore_count() const -> std::size_t {
    return app_.pending_cellapp_death_restores_.size();
  }
  void seed_death_restore_counters(uint64_t notifications, uint64_t scheduled, uint64_t restored,
                                   uint64_t lost, uint64_t timeouts) {
    app_.cellapp_death_notifications_total_ = notifications;
    app_.cellapp_death_restore_scheduled_total_ = scheduled;
    app_.cellapp_death_restore_payload_scheduled_total_ = scheduled;
    app_.cellapp_death_restore_ghost_backup_scheduled_total_ = 0;
    app_.cellapp_death_restored_total_ = restored;
    app_.cellapp_death_lost_total_ = lost;
    app_.cellapp_death_restore_timeouts_total_ = timeouts;
  }
  void seed_pending_death_restore(EntityID entity_id, TimePoint started_at = Clock::now()) {
    app_.pending_cellapp_death_restores_[entity_id] =
        BaseApp::PendingCellAppDeathRestore{started_at, Address(0x7F000001u, 30002)};
  }
  auto expired_death_restore_start() const -> TimePoint {
    return Clock::now() - BaseApp::kCellAppDeathRestoreTimeout - std::chrono::seconds(1);
  }
  void on_cell_entity_created(const baseapp::CellEntityCreated& msg) {
    app_.OnCellEntityCreated(*test_support::FakeChannel(0xCA11), msg);
  }
  void on_cell_entity_create_failed(const baseapp::CellEntityCreateFailed& msg) {
    app_.OnCellEntityCreateFailed(msg);
  }
  void on_cellapp_death(const baseapp::CellAppDeath& death) { app_.OnCellAppDeath(death); }
  void sweep_death_restore_timeouts() { app_.SweepCellAppDeathRestoreTimeouts(); }

  EventDispatcher dispatcher_;
  NetworkInterface internal_network_;
  NetworkInterface external_network_;
  BaseApp app_;
  bool ids_seeded_{false};
  bool checkout_type_registered_{false};
};

TEST_F(BaseAppRollbackTest, NativeDeltaTimeUsesTheAppGameClock) {
  BaseAppNativeProvider provider(app_);
  auto& clock = app_.GetGameClock();
  EXPECT_FLOAT_EQ(provider.DeltaTime(), 0.f);
  clock.Tick(std::chrono::milliseconds(25));
  EXPECT_FLOAT_EQ(provider.DeltaTime(), 0.025f);
  clock.SetTimeScale(0.5);
  clock.Tick(std::chrono::milliseconds(40));
  EXPECT_FLOAT_EQ(provider.DeltaTime(), 0.02f);
  clock.SetTimeScale(0.0);
  clock.Tick(std::chrono::milliseconds(40));
  EXPECT_FLOAT_EQ(provider.DeltaTime(), 0.f);
  clock.Reset();
  EXPECT_FLOAT_EQ(provider.DeltaTime(), 0.f);
}

TEST_F(BaseAppRollbackTest, SpaceReplyMustMatchManagerChannelAndSpaceAndCompletesOnce) {
  InterfaceTable table;
  auto manager = make_intrusive<CapturingChannel>(dispatcher_, table, Address("127.0.0.1", 26001));
  auto impostor = make_intrusive<CapturingChannel>(dispatcher_, table, manager->RemoteAddress());
  set_space_manager(manager.get());
  int completed = 0;
  const auto id = request_space(99, [&](bool success, SpaceID space, const Address& host) {
    EXPECT_TRUE(success);
    EXPECT_EQ(space, 99u);
    EXPECT_EQ(host.Port(), 26002u);
    ++completed;
  });
  ASSERT_NE(id, 0u);
  cellappmgr::SpaceCreatedResult reply{id, 99, true, 1, Address("127.0.0.1", 26002)};
  answer_space(*impostor, reply);
  reply.space_id = 100;
  answer_space(*manager, reply);
  EXPECT_EQ(completed, 0);
  EXPECT_EQ(pending_spaces(), 1u);
  reply.space_id = 99;
  answer_space(*manager, reply);
  answer_space(*manager, reply);
  EXPECT_EQ(completed, 1);
  EXPECT_EQ(pending_spaces(), 0u);
}

TEST_F(BaseAppRollbackTest, ShutdownRejectsNewSpaceRequestsBeforeSending) {
  InterfaceTable table;
  auto manager = make_intrusive<CapturingChannel>(dispatcher_, table, Address("127.0.0.1", 26001));
  set_space_manager(manager.get());
  begin_shutdown();
  int completed = 0;
  EXPECT_EQ(request_space(99,
                          [&](bool success, SpaceID, const Address&) {
                            EXPECT_FALSE(success);
                            ++completed;
                          }),
            0u);
  EXPECT_EQ(completed, 1);
  EXPECT_EQ(pending_spaces(), 0u);
  EXPECT_EQ(manager->send_count(), 0u);
}

TEST_F(BaseAppRollbackTest, SpaceTimeoutDisconnectSendFailureAndExhaustionReportOriginalSpace) {
  InterfaceTable table;
  auto manager = make_intrusive<CapturingChannel>(dispatcher_, table, Address("127.0.0.1", 26001));
  set_space_manager(manager.get());
  int failures = 0;
  auto failed = [&](bool success, SpaceID space, const Address& host) {
    EXPECT_FALSE(success);
    EXPECT_EQ(space, 99u);
    EXPECT_EQ(host, Address{});
    ++failures;
  };
  ASSERT_NE(request_space(99, failed), 0u);
  expire_spaces();
  expire_spaces();
  EXPECT_EQ(failures, 1);
  ASSERT_NE(request_space(99, failed), 0u);
  fail_spaces();
  fail_spaces();
  EXPECT_EQ(failures, 2);
  manager->FailNextSend();
  EXPECT_EQ(request_space(99, failed), 0u);
  EXPECT_EQ(failures, 3);
  exhaust_space_ids();
  EXPECT_EQ(request_space(99, failed), 0u);
  EXPECT_EQ(failures, 4);
  EXPECT_EQ(pending_spaces(), 0u);
}

TEST_F(BaseAppRollbackTest, SpaceCreationRefusesOverloadWithoutAddingWaiters) {
  InterfaceTable table;
  auto manager = make_intrusive<CapturingChannel>(dispatcher_, table, Address("127.0.0.1", 26001));
  set_space_manager(manager.get());
  int failed = 0;
  auto callback = [&](bool success, SpaceID, const Address&) {
    EXPECT_FALSE(success);
    ++failed;
  };
  for (SpaceID id = 1; id <= 1024; ++id) ASSERT_NE(request_space(id, callback), 0u);
  EXPECT_EQ(request_space(1025, callback), 0u);
  EXPECT_EQ(pending_spaces(), 1024u);
  EXPECT_EQ(manager->send_count(), 1024u);
  EXPECT_EQ(failed, 1);
  fail_spaces();
  EXPECT_EQ(failed, 1025);
}

class ClientHandoffTest : public BaseAppRollbackTest {
 protected:
  void SetUp() override {
    target_ = new_proxy();
    ASSERT_NE(target_, nullptr);
    auto key = SessionKey::Generate();
    ASSERT_TRUE(key.HasValue());
    credential_ = *key;
    request_ = handoff_request(*target_, credential_);
    database_ = make_intrusive<CapturingChannel>(dispatcher_, table_, Address("127.0.0.1", 24001));
    source_ =
        make_intrusive<CapturingChannel>(dispatcher_, table_, request_.expected.owner.base_addr);
    client_ = make_intrusive<CapturingChannel>(dispatcher_, table_, Address("127.0.0.1", 28001));
    set_legacy_dbapp_channel(database_.get());
    retain_client_channel(client_->RemoteAddress());
  }
  auto accept_target() -> AccountSession {
    prepare_handoff(*source_, request_);
    answer_session(*database_, request_.expected);
    auto accepted = apply_latest(*database_, request_.expected);
    answer_session(*database_, accepted);
    return accepted;
  }
  auto awaiting_client(AccountSession record) -> AccountSession {
    record.phase = AccountSessionPhase::kAwaitingClient;
    ++record.version;
    return record;
  }
  auto handoff_replies() const -> std::vector<baseapp::PrepareClientHandoffReply> {
    return DecodeSentMessages<baseapp::PrepareClientHandoffReply>(*source_);
  }
  InterfaceTable table_;
  IntrusivePtr<CapturingChannel> database_;
  IntrusivePtr<CapturingChannel> source_;
  IntrusivePtr<CapturingChannel> client_;
  Proxy* target_{nullptr};
  SessionKey credential_;
  baseapp::PrepareClientHandoff request_;
};

TEST_F(ClientHandoffTest, AcceptanceWaitsForCommitAndDoesNotBind) {
  prepare_handoff(*source_, request_);
  EXPECT_TRUE(handoff_replies().empty());
  EXPECT_TRUE(transfer_target_reserved(target_->EntityId()));
  EXPECT_FALSE(finalize_proxy(target_->EntityId()));
  answer_session(*database_, request_.expected);
  EXPECT_TRUE(handoff_replies().empty());
  const auto accepted = apply_latest(*database_, request_.expected);
  EXPECT_EQ(accepted.phase, AccountSessionPhase::kAccepted);
  answer_session(*database_, accepted);
  ASSERT_EQ(handoff_replies().size(), 1u);
  EXPECT_EQ(handoff_replies().front().status, ErrorCode::kNone);
  EXPECT_FALSE(target_->HasClient());
  EXPECT_TRUE(target_->GetSessionKey().IsZero());
}

TEST_F(ClientHandoffTest, LegacyAcceptMessageCannotOverwriteAnotherProxysSessionKey) {
  auto* active = prepare_auth_proxy(43);
  ASSERT_NE(active, nullptr);
  const auto original = active->GetSessionKey();
  BinaryWriter payload;
  payload.Write(active->EntityId());
  payload.WriteBytes(std::as_bytes(std::span{credential_.bytes}));
  EXPECT_FALSE(dispatch_internal(*source_, 2002, payload.Data()));
  EXPECT_TRUE(active->GetSessionKey() == original);
}

TEST_F(ClientHandoffTest, InvalidProcessAndDefinitionDoNotContactDatabase) {
  auto invalid = request_;
  ++invalid.requester_instance;
  prepare_handoff(*source_, invalid);
  invalid = request_;
  invalid.entity_def_digest[0] ^= std::byte{1};
  prepare_handoff(*source_, invalid);
  invalid = request_;
  ++invalid.expected.target.process_instance;
  prepare_handoff(*source_, invalid);
  EXPECT_EQ(database_->send_count(), 0u);
  EXPECT_EQ(incoming_handoff_count(), 0u);
  ASSERT_EQ(handoff_replies().size(), 3u);
  for (const auto& reply : handoff_replies()) EXPECT_NE(reply.status, ErrorCode::kNone);
}

TEST_F(ClientHandoffTest, OccupiedAndPreparedTargetsAreRejected) {
  seed_expired_prepared_login(100, target_->EntityId());
  prepare_handoff(*source_, request_);
  ASSERT_EQ(handoff_replies().size(), 1u);
  EXPECT_EQ(handoff_replies().front().status, ErrorCode::kAlreadyExists);
  EXPECT_EQ(database_->send_count(), 0u);
}

TEST_F(ClientHandoffTest, QueueLimitRejectsAnotherTargetWithoutSending) {
  (void)accept_target();
  fill_handoff_queue();
  auto* another = new_proxy();
  ASSERT_NE(another, nullptr);
  auto request = request_;
  request.expected.target.entity_id = another->EntityId();
  const auto sends = database_->send_count();
  prepare_handoff(*source_, request);
  EXPECT_EQ(handoff_replies().back().status, ErrorCode::kRateLimited);
  EXPECT_EQ(database_->send_count(), sends);
  EXPECT_EQ(incoming_handoff_count(), 1024u);
}

TEST_F(ClientHandoffTest, OccupiedTargetCannotBeAccepted) {
  target_->BindClient(client_->RemoteAddress());
  prepare_handoff(*source_, request_);
  EXPECT_EQ(handoff_replies().back().status, ErrorCode::kAlreadyExists);
  EXPECT_EQ(database_->send_count(), 0u);
  EXPECT_TRUE(target_->HasClient());
}

TEST_F(ClientHandoffTest, DuplicateWhilePendingIsBoundedAndConfirmedReplayDoesNotMutate) {
  prepare_handoff(*source_, request_);
  prepare_handoff(*source_, request_);
  EXPECT_EQ(DecodeSentMessages<dbapp::AccountSessionRequest>(*database_).size(), 1u);
  ASSERT_EQ(handoff_replies().size(), 1u);
  EXPECT_EQ(handoff_replies().back().status, ErrorCode::kWouldBlock);
  answer_session(*database_, request_.expected);
  const auto accepted = apply_latest(*database_, request_.expected);
  answer_session(*database_, accepted);
  prepare_handoff(*source_, request_);
  auto reads = DecodeSentMessages<dbapp::AccountSessionRequest>(*database_);
  ASSERT_EQ(reads.size(), 3u);
  EXPECT_FALSE(reads.back().operation.has_value());
  answer_session(*database_, accepted);
  EXPECT_EQ(handoff_replies().back().status, ErrorCode::kNone);
  EXPECT_EQ(incoming_handoff_count(), 1u);
}

TEST_F(ClientHandoffTest, AcceptanceCommitUnknownKeepsTargetReservedAndUnbound) {
  prepare_handoff(*source_, request_);
  answer_session(*database_, request_.expected);
  const auto accepted = apply_latest(*database_, request_.expected);
  answer_session(*database_, {}, ErrorCode::kCommitUnknown);
  ASSERT_EQ(handoff_replies().size(), 1u);
  EXPECT_EQ(handoff_replies().back().status, ErrorCode::kCommitUnknown);
  EXPECT_TRUE(transfer_target_reserved(target_->EntityId()));
  EXPECT_FALSE(target_->HasClient());
  prepare_handoff(*source_, request_);
  answer_session(*database_, accepted);
  EXPECT_EQ(handoff_replies().back().status, ErrorCode::kNone);
}

TEST_F(ClientHandoffTest, FailedInitialReadDoesNotLeaveAnUnusedReservation) {
  prepare_handoff(*source_, request_);
  answer_session(*database_, {}, ErrorCode::kNotFound);
  EXPECT_EQ(handoff_replies().back().status, ErrorCode::kNotFound);
  EXPECT_EQ(incoming_handoff_count(), 0u);
  EXPECT_FALSE(transfer_target_reserved(target_->EntityId()));
  EXPECT_EQ(session_pending_count(), 0u);
  EXPECT_FALSE(target_->HasClient());
}

TEST_F(ClientHandoffTest, ReauthenticationOnlyBindsAfterCompleteCommit) {
  const auto accepted = accept_target();
  const auto awaiting = awaiting_client(accepted);
  authenticate(*client_, credential_);
  EXPECT_FALSE(target_->HasClient());
  answer_session(*database_, awaiting);
  const auto active = apply_latest(*database_, awaiting);
  EXPECT_EQ(active.owner, request_.expected.target);
  EXPECT_FALSE(target_->HasClient());
  EXPECT_TRUE(DecodeSentMessages<baseapp::AuthenticateResult>(*client_).empty());
  answer_session(*database_, active);
  const auto replies = DecodeSentMessages<baseapp::AuthenticateResult>(*client_);
  ASSERT_EQ(replies.size(), 1u);
  EXPECT_TRUE(replies.front().success);
  EXPECT_TRUE(target_->HasClient());
  EXPECT_FALSE(session_frozen(target_->EntityId()));
  EXPECT_EQ(incoming_handoff_count(), 0u);
  EXPECT_FALSE(transfer_target_reserved(target_->EntityId()));
}

TEST_F(ClientHandoffTest, CredentialAndAwaitingPhaseAreBothRequired) {
  const auto accepted = accept_target();
  auto wrong = credential_;
  wrong.bytes[0] ^= 1;
  const auto sends = database_->send_count();
  authenticate(*client_, wrong);
  EXPECT_EQ(database_->send_count(), sends);
  authenticate(*client_, credential_);
  answer_session(*database_, accepted);
  const auto requests = DecodeSentMessages<dbapp::AccountSessionRequest>(*database_);
  EXPECT_FALSE(requests.back().operation.has_value());
  EXPECT_FALSE(target_->HasClient());
  const auto replies = DecodeSentMessages<baseapp::AuthenticateResult>(*client_);
  ASSERT_EQ(replies.size(), 2u);
  EXPECT_FALSE(replies[0].success);
  EXPECT_FALSE(replies[1].success);
}

TEST_F(ClientHandoffTest, EmptyCredentialDoesNotInitiateAConfirmation) {
  credential_ = {};
  const auto hash = Sha256(std::as_bytes(std::span{credential_.bytes}));
  ASSERT_TRUE(hash.HasValue());
  request_.expected.credential_hash = *hash;
  (void)accept_target();
  const auto sends = database_->send_count();
  authenticate(*client_, credential_);
  EXPECT_EQ(database_->send_count(), sends);
  EXPECT_FALSE(target_->HasClient());
  EXPECT_FALSE(DecodeSentMessages<baseapp::AuthenticateResult>(*client_).back().success);
}

TEST_F(ClientHandoffTest, WaiterCompletesOnlyAfterBindingAndDuplicateWaitIsIgnored) {
  const auto awaiting = awaiting_client(accept_target());
  request_.wait_for_client = true;
  ++request_.request_id;
  prepare_handoff(*source_, request_);
  prepare_handoff(*source_, request_);
  EXPECT_EQ(handoff_replies().size(), 1u);
  authenticate(*client_, credential_);
  answer_session(*database_, awaiting);
  const auto active = apply_latest(*database_, awaiting);
  EXPECT_EQ(handoff_replies().size(), 1u);
  answer_session(*database_, active);
  ASSERT_EQ(handoff_replies().size(), 2u);
  EXPECT_EQ(handoff_replies().back().request_id, request_.request_id);
  EXPECT_EQ(handoff_replies().back().status, ErrorCode::kNone);
  EXPECT_TRUE(target_->HasClient());
  prepare_handoff(*source_, request_);
  EXPECT_EQ(handoff_replies().size(), 3u);
  EXPECT_EQ(handoff_replies().back().status, ErrorCode::kNone);
  auto forged = request_;
  forged.expected.owner = {Address("127.0.0.1", 25003), 600, 333};
  forged.requester_instance = 333;
  register_base_peer(forged.expected.owner.base_addr, 333);
  auto other =
      make_intrusive<CapturingChannel>(dispatcher_, table_, forged.expected.owner.base_addr);
  prepare_handoff(*other, forged);
  const auto replies = DecodeSentMessages<baseapp::PrepareClientHandoffReply>(*other);
  ASSERT_EQ(replies.size(), 1u);
  EXPECT_NE(replies.back().status, ErrorCode::kNone);
}

TEST_F(ClientHandoffTest, ExpiredWaiterCannotReportSuccessfulAuthentication) {
  const auto accepted = accept_target();
  request_.wait_for_client = true;
  ++request_.request_id;
  prepare_handoff(*source_, request_);
  expire_handoffs();
  answer_session(*database_, accepted);
  answer_session(*database_, accepted);
  const auto aborted = apply_latest(*database_, accepted);
  answer_session(*database_, aborted);
  ASSERT_EQ(handoff_replies().size(), 2u);
  EXPECT_NE(handoff_replies().back().status, ErrorCode::kNone);
  EXPECT_FALSE(target_->HasClient());
  EXPECT_EQ(incoming_handoff_count(), 0u);
}

TEST_F(ClientHandoffTest, ChangedHandoffCannotGrantControl) {
  auto awaiting = awaiting_client(accept_target());
  awaiting.handoff_id[0] ^= std::byte{2};
  authenticate(*client_, credential_);
  answer_session(*database_, awaiting);
  EXPECT_FALSE(DecodeSentMessages<dbapp::AccountSessionRequest>(*database_).back().operation);
  EXPECT_FALSE(target_->HasClient());
  EXPECT_FALSE(DecodeSentMessages<baseapp::AuthenticateResult>(*client_).back().success);
}

TEST_F(ClientHandoffTest, ChangedDatabasePreparationCannotBeAccepted) {
  prepare_handoff(*source_, request_);
  auto changed = request_.expected;
  ++changed.version;
  changed.credential_hash[0] ^= std::byte{1};
  answer_session(*database_, changed);
  EXPECT_EQ(handoff_replies().back().status, ErrorCode::kPermissionDenied);
  EXPECT_FALSE(DecodeSentMessages<dbapp::AccountSessionRequest>(*database_).back().operation);
  EXPECT_FALSE(target_->HasClient());
}

TEST_F(ClientHandoffTest, LateAcceptanceCannotReviveDestroyedTarget) {
  prepare_handoff(*source_, request_);
  answer_session(*database_, request_.expected);
  const auto accepted = apply_latest(*database_, request_.expected);
  target_->MarkForDestroy();
  answer_session(*database_, accepted);
  EXPECT_EQ(handoff_replies().back().status, ErrorCode::kCommitUnknown);
  authenticate(*client_, credential_);
  EXPECT_FALSE(target_->HasClient());
  EXPECT_FALSE(DecodeSentMessages<baseapp::AuthenticateResult>(*client_).back().success);
}

TEST_F(ClientHandoffTest, CompleteUnknownRetainsOperationForCredentialRetry) {
  const auto awaiting = awaiting_client(accept_target());
  authenticate(*client_, credential_);
  answer_session(*database_, awaiting);
  const auto active = apply_latest(*database_, awaiting);
  const auto original = DecodeSentMessages<dbapp::AccountSessionRequest>(*database_).back();
  answer_session(*database_, {}, ErrorCode::kCommitUnknown);
  EXPECT_FALSE(target_->HasClient());
  EXPECT_TRUE(transfer_target_reserved(target_->EntityId()));
  authenticate(*client_, credential_);
  const auto replay = DecodeSentMessages<dbapp::AccountSessionRequest>(*database_).back();
  ASSERT_TRUE(replay.operation.has_value());
  ASSERT_TRUE(original.operation.has_value());
  EXPECT_EQ(replay.operation->id, original.operation->id);
  answer_session(*database_, active);
  EXPECT_TRUE(target_->HasClient());
  EXPECT_EQ(incoming_handoff_count(), 0u);
}

TEST_F(ClientHandoffTest, ConcurrentCredentialRequestsDoNotCreateTwoCompletions) {
  const auto awaiting = awaiting_client(accept_target());
  authenticate(*client_, credential_);
  const auto sends = database_->send_count();
  authenticate(*client_, credential_);
  EXPECT_EQ(database_->send_count(), sends);
  answer_session(*database_, awaiting);
  const auto active = apply_latest(*database_, awaiting);
  answer_session(*database_, active);
  const auto replies = DecodeSentMessages<baseapp::AuthenticateResult>(*client_);
  ASSERT_EQ(replies.size(), 2u);
  EXPECT_FALSE(replies.front().success);
  EXPECT_TRUE(replies.back().success);
}

TEST_F(ClientHandoffTest, DisconnectedClientAfterCompleteDoesNotBindAndExpiresSafely) {
  const auto awaiting = awaiting_client(accept_target());
  authenticate(*client_, credential_);
  answer_session(*database_, awaiting);
  const auto active = apply_latest(*database_, awaiting);
  client_->Condemn();
  answer_session(*database_, active);
  EXPECT_FALSE(target_->HasClient());
  expire_handoffs();
  answer_session(*database_, active);
  answer_session(*database_, active);
  const auto offline = apply_latest(*database_, active);
  EXPECT_EQ(offline.phase, AccountSessionPhase::kOffline);
  answer_session(*database_, offline);
  EXPECT_EQ(incoming_handoff_count(), 0u);
  EXPECT_FALSE(transfer_target_reserved(target_->EntityId()));
}

TEST_F(ClientHandoffTest, ExpirationWaitsForAbortConfirmationAndRetainsUnknownState) {
  const auto accepted = accept_target();
  expire_handoffs();
  answer_session(*database_, accepted);
  answer_session(*database_, accepted);
  const auto aborted = apply_latest(*database_, accepted);
  EXPECT_EQ(aborted.owner, request_.expected.owner);
  EXPECT_EQ(aborted.phase, AccountSessionPhase::kActive);
  answer_session(*database_, {}, ErrorCode::kCommitUnknown);
  EXPECT_EQ(incoming_handoff_count(), 1u);
  EXPECT_TRUE(transfer_target_reserved(target_->EntityId()));
  expire_handoffs();
  answer_session(*database_, aborted);
  EXPECT_EQ(incoming_handoff_count(), 0u);
  EXPECT_FALSE(target_->HasClient());
}

TEST_F(ClientHandoffTest, ExpirationCanCleanADestroyedOrCancelledCandidate) {
  const auto accepted = accept_target();
  target_->MarkForDestroy();
  auto released = std::make_shared<std::optional<Result<void>>>();
  release_session(target_->EntityId(), released);
  answer_session(*database_, {}, ErrorCode::kReceiverGone);
  ASSERT_TRUE(released->has_value());
  EXPECT_FALSE((*released)->HasValue());
  expire_handoffs();
  answer_session(*database_, accepted);
  answer_session(*database_, accepted);
  const auto aborted = apply_latest(*database_, accepted);
  answer_session(*database_, aborted);
  EXPECT_EQ(incoming_handoff_count(), 0u);
  EXPECT_FALSE(transfer_target_reserved(target_->EntityId()));
  EXPECT_FALSE(target_->HasClient());
}

TEST_F(ClientHandoffTest, RevocationDuringAuthenticationPreventsLateBinding) {
  const auto awaiting = awaiting_client(accept_target());
  authenticate(*client_, credential_);
  auto released = std::make_shared<std::optional<Result<void>>>();
  release_session(target_->EntityId(), released);
  answer_session(*database_, awaiting);
  EXPECT_FALSE(released->has_value());
  const auto aborted = apply_latest(*database_, awaiting);
  answer_session(*database_, aborted);
  ASSERT_TRUE(released->has_value());
  EXPECT_TRUE((*released)->HasValue());
  EXPECT_EQ(incoming_handoff_count(), 0u);
  EXPECT_FALSE(target_->HasClient());
  EXPECT_EQ(session_pending_count(), 0u);
}

TEST_F(ClientHandoffTest, RevocationResolvesUncertainCompleteBeforeReleasing) {
  const auto awaiting = awaiting_client(accept_target());
  authenticate(*client_, credential_);
  answer_session(*database_, awaiting);
  const auto active = apply_latest(*database_, awaiting);
  const auto completion = DecodeSentMessages<dbapp::AccountSessionRequest>(*database_).back();
  auto released = std::make_shared<std::optional<Result<void>>>();
  release_session(target_->EntityId(), released);
  const auto replay = DecodeSentMessages<dbapp::AccountSessionRequest>(*database_).back();
  ASSERT_TRUE(replay.operation.has_value());
  ASSERT_TRUE(completion.operation.has_value());
  EXPECT_EQ(replay.operation->id, completion.operation->id);
  answer_session(*database_, active);
  EXPECT_FALSE(target_->HasClient());
  answer_session(*database_, active);
  const auto offline = apply_latest(*database_, active);
  answer_session(*database_, offline);
  ASSERT_TRUE(released->has_value());
  EXPECT_TRUE((*released)->HasValue());
  EXPECT_EQ(incoming_handoff_count(), 0u);
  EXPECT_EQ(session_pending_count(), 0u);
  EXPECT_FALSE(target_->HasClient());
}

TEST_F(BaseAppRollbackTest, HandoffTargetRequiresARegisteredInstanceAndLiveProxy) {
  ASSERT_TRUE(internal_network_.StartRudpServer(Address("127.0.0.1", 0)));
  auto* proxy = new_proxy();
  ASSERT_NE(proxy, nullptr);
  EXPECT_FALSE(handoff_target(proxy->EntityId()));
  const auto address = internal_network_.RudpAddress();
  register_base_peer(address, 111);
  const auto target = handoff_target(proxy->EntityId());
  ASSERT_TRUE(target.has_value());
  EXPECT_EQ(target->base_addr, address);
  EXPECT_EQ(target->process_instance, 111u);
  EXPECT_EQ(target->entity_id, proxy->EntityId());
  EXPECT_FALSE(handoff_target(0));
  proxy->MarkForDestroy();
  EXPECT_FALSE(handoff_target(proxy->EntityId()));
  auto* next = new_proxy();
  ASSERT_NE(next, nullptr);
  retire_base_peer(address, 111);
  EXPECT_FALSE(handoff_target(next->EntityId()));
}

TEST_F(BaseAppRollbackTest, BasePeerDeathCannotEraseAReplacementInstance) {
  const Address address("127.0.0.1", 25002);
  register_base_peer(address, 222);
  register_base_peer(address, 333);
  retire_base_peer(address, 222);
  EXPECT_EQ(base_peer_instance(address), 333u);
  retire_base_peer(address, 333);
  EXPECT_EQ(base_peer_instance(address), 0u);
}

TEST_F(BaseAppRollbackTest, RemoteHandoffAcceptanceRoundTripRequiresCommitAndReplyProcess) {
  check_handoff_roundtrip();
}

TEST_F(BaseAppRollbackTest, RemoteHandoffKeepsSourceBoundUntilTargetAuthentication) {
  check_outgoing_handoff();
}

TEST_F(BaseAppRollbackTest, RemoteHandoffReleasesSourceAfterRecoveryGrantAdvancesVersion) {
  check_outgoing_handoff(false, false, false, true);
}

TEST_F(BaseAppRollbackTest, RejectedRemoteHandoffRestoresOnlyConfirmedSourceOwnership) {
  check_outgoing_handoff(true);
}

TEST_F(BaseAppRollbackTest, UnknownRemoteHandoffDoesNotGuessAControlOwner) {
  check_outgoing_handoff(false, true);
}

TEST_F(BaseAppRollbackTest, UnsupportedClientCannotStartRemoteHandoff) {
  check_outgoing_handoff(false, false, true);
}

TEST_F(BaseAppRollbackTest, RemoteSessionRevocationRoundTripGatesNewLoginCheckout) {
  check_remote_revocation_roundtrip();
}

TEST_F(BaseAppRollbackTest, SessionRevocationRejectsUnregisteredAndReplacedRequester) {
  InterfaceTable table;
  auto peer = make_intrusive<CapturingChannel>(dispatcher_, table, Address("127.0.0.1", 25002));
  auto database = make_intrusive<CapturingChannel>(dispatcher_, table, Address("127.0.0.1", 24002));
  set_legacy_dbapp_channel(database.get());
  register_base_peer(Address("127.0.0.1", 25001), 111);
  baseapp::RevokeAccountSession request{
      (uint64_t{1} << 40) + 1, 222, 42, 7, 9, {Address("127.0.0.1", 25001), 100, 111}};
  revoke_session(*peer, request);
  register_base_peer(peer->RemoteAddress(), 333);
  revoke_session(*peer, request);
  EXPECT_EQ(database->send_count(), 0u);
  auto replies = DecodeSentMessages<baseapp::RevokeAccountSessionReply>(*peer);
  ASSERT_EQ(replies.size(), 2u);
  for (const auto& reply : replies) EXPECT_EQ(reply.status, ErrorCode::kPermissionDenied);
  EXPECT_EQ(revocation_count(), 0u);
}

TEST_F(BaseAppRollbackTest, SessionRevocationChecksVersionAndWaitsForDatabaseCommit) {
  InterfaceTable table;
  auto peer = make_intrusive<CapturingChannel>(dispatcher_, table, Address("127.0.0.1", 25002));
  auto database = make_intrusive<CapturingChannel>(dispatcher_, table, Address("127.0.0.1", 24002));
  set_legacy_dbapp_channel(database.get());
  register_base_peer(peer->RemoteAddress(), 222);
  register_base_peer(Address("127.0.0.1", 25001), 111);
  baseapp::RevokeAccountSession request{
      (uint64_t{1} << 40) + 1, 222, 42, 7, 9, {Address("127.0.0.1", 25001), 100, 111}};
  AccountSession record;
  record.version = 10;
  record.phase = AccountSessionPhase::kActive;
  record.owner = request.owner;
  revoke_session(*peer, request);
  EXPECT_EQ(peer->send_count(), 0u);
  answer_session(*database, record);
  auto replies = DecodeSentMessages<baseapp::RevokeAccountSessionReply>(*peer);
  ASSERT_EQ(replies.size(), 1u);
  EXPECT_EQ(replies[0].status, ErrorCode::kPermissionDenied);
  request.request_id++;
  request.version = record.version;
  revoke_session(*peer, request);
  answer_session(*database, record);
  EXPECT_EQ(DecodeSentMessages<baseapp::RevokeAccountSessionReply>(*peer).size(), 1u);
  auto writes = DecodeSentMessages<dbapp::AccountSessionRequest>(*database);
  ASSERT_TRUE(writes.back().operation.has_value());
  EXPECT_EQ(writes.back().operation->expected_version, 10u);
  EXPECT_EQ(writes.back().operation->action, AccountSessionAction::kRelease);
  answer_session(*database, {}, ErrorCode::kCommitUnknown);
  replies = DecodeSentMessages<baseapp::RevokeAccountSessionReply>(*peer);
  ASSERT_EQ(replies.size(), 2u);
  EXPECT_EQ(replies.back().status, ErrorCode::kCommitUnknown);
  EXPECT_EQ(replies.back().version, 10u);
  EXPECT_EQ(revocation_count(), 0u);
  request.request_id++;
  revoke_session(*peer, request);
  AccountSession offline;
  offline.version = 11;
  answer_session(*database, offline);
  replies = DecodeSentMessages<baseapp::RevokeAccountSessionReply>(*peer);
  ASSERT_EQ(replies.size(), 3u);
  EXPECT_EQ(replies.back().status, ErrorCode::kNone);
}

TEST_F(BaseAppRollbackTest, SessionRevocationFreezesAndReleasesTheBoundOwner) {
  InterfaceTable table;
  auto peer = make_intrusive<CapturingChannel>(dispatcher_, table, Address("127.0.0.1", 25002));
  auto database = make_intrusive<CapturingChannel>(dispatcher_, table, Address("127.0.0.1", 24002));
  auto client = make_intrusive<CapturingChannel>(dispatcher_, table, Address("127.0.0.1", 27001));
  set_legacy_dbapp_channel(database.get());
  register_base_peer(peer->RemoteAddress(), 222);
  register_base_peer(Address("127.0.0.1", 25001), 111);
  auto* proxy = prepare_auth_proxy();
  ASSERT_NE(proxy, nullptr);
  authenticate(*client, proxy->GetSessionKey());
  answer_session(*database, {});
  const auto grant = active_session(*proxy);
  answer_session(*database, grant);
  ASSERT_TRUE(proxy->HasClient());
  baseapp::RevokeAccountSession request{(uint64_t{1} << 40) + 1, 222,        42, 7,
                                        grant.version,           grant.owner};
  revoke_session(*peer, request);
  answer_session(*database, grant);
  EXPECT_TRUE(session_frozen(proxy->EntityId()));
  EXPECT_TRUE(proxy->HasClient());
  answer_session(*database, grant);
  EXPECT_EQ(peer->send_count(), 0u);
  AccountSession offline;
  offline.version = 2;
  answer_session(*database, offline);
  EXPECT_FALSE(proxy->HasClient());
  EXPECT_TRUE(proxy->GetSessionKey().IsZero());
  auto replies = DecodeSentMessages<baseapp::RevokeAccountSessionReply>(*peer);
  ASSERT_EQ(replies.size(), 1u);
  EXPECT_EQ(replies[0].status, ErrorCode::kNone);
  EXPECT_EQ(revocation_count(), 0u);
}

TEST_F(BaseAppRollbackTest, SessionRevocationRejectsOverloadWithoutDatabaseWork) {
  InterfaceTable table;
  auto peer = make_intrusive<CapturingChannel>(dispatcher_, table, Address("127.0.0.1", 25002));
  auto database = make_intrusive<CapturingChannel>(dispatcher_, table, Address("127.0.0.1", 24002));
  set_legacy_dbapp_channel(database.get());
  register_base_peer(peer->RemoteAddress(), 222);
  register_base_peer(Address("127.0.0.1", 25001), 111);
  baseapp::RevokeAccountSession request{7, 222, 42, 7, 9, {Address("127.0.0.1", 25001), 100, 111}};
  set_revocations_full(true);
  revoke_session(*peer, request);
  set_revocations_full(false);
  EXPECT_EQ(database->send_count(), 0u);
  auto replies = DecodeSentMessages<baseapp::RevokeAccountSessionReply>(*peer);
  ASSERT_EQ(replies.size(), 1u);
  EXPECT_EQ(replies[0].status, ErrorCode::kRateLimited);
}

TEST_F(BaseAppRollbackTest, LoginWaitsForSessionCheckBeforeCheckout) {
  InterfaceTable table;
  auto database = make_intrusive<CapturingChannel>(dispatcher_, table, Address("127.0.0.1", 24002));
  auto login = make_intrusive<CapturingChannel>(dispatcher_, table, Address("127.0.0.1", 28001));
  set_legacy_dbapp_channel(database.get());
  prepare_login(*login);
  EXPECT_EQ(login_session_check_count(), 1u);
  EXPECT_TRUE(DecodeSentMessages<dbapp::CheckoutEntity>(*database).empty());
  answer_session(*database, {});
  EXPECT_EQ(login_session_check_count(), 0u);
  EXPECT_EQ(DecodeSentMessages<dbapp::CheckoutEntity>(*database).size(), 1u);
}

TEST_F(BaseAppRollbackTest, LoginCancellationChecksItsSourceAndRemovesSessionWait) {
  InterfaceTable table;
  auto database = make_intrusive<CapturingChannel>(dispatcher_, table, Address("127.0.0.1", 24002));
  auto login = make_intrusive<CapturingChannel>(dispatcher_, table, Address("127.0.0.1", 28001));
  auto wrong = make_intrusive<CapturingChannel>(dispatcher_, table, Address("127.0.0.1", 28002));
  set_legacy_dbapp_channel(database.get());
  prepare_login(*login);
  cancel_login(*wrong);
  EXPECT_EQ(login_session_check_count(), 1u);
  cancel_login(*login);
  EXPECT_EQ(login_session_check_count(), 0u);
  EXPECT_EQ(session_pending_count(), 0u);
  answer_session(*database, {});
  EXPECT_TRUE(DecodeSentMessages<dbapp::CheckoutEntity>(*database).empty());
}

TEST_F(BaseAppRollbackTest, LoginDoesNotGuessThatAnUnavailableOwnerIsOffline) {
  InterfaceTable table;
  auto database = make_intrusive<CapturingChannel>(dispatcher_, table, Address("127.0.0.1", 24002));
  auto login = make_intrusive<CapturingChannel>(dispatcher_, table, Address("127.0.0.1", 28001));
  set_legacy_dbapp_channel(database.get());
  prepare_login(*login);
  AccountSession record;
  record.version = 9;
  record.phase = AccountSessionPhase::kActive;
  record.owner = {Address("127.0.0.1", 25002), 100, 222};
  answer_session(*database, record);
  EXPECT_EQ(login_session_check_count(), 0u);
  EXPECT_TRUE(DecodeSentMessages<dbapp::CheckoutEntity>(*database).empty());
  EXPECT_EQ(session_pending_count(), 0u);
}

TEST_F(BaseAppRollbackTest, RecoveryCredentialIsDeliveredOnlyAfterDatabaseConfirmation) {
  InterfaceTable table;
  auto database = make_intrusive<CapturingChannel>(dispatcher_, table, Address("127.0.0.1", 24002));
  auto client = make_intrusive<CapturingChannel>(dispatcher_, table, Address("127.0.0.1", 27001));
  set_legacy_dbapp_channel(database.get());
  auto* proxy = prepare_auth_proxy();
  ASSERT_NE(proxy, nullptr);
  authenticate(*client, proxy->GetSessionKey(), true, true);
  answer_session(*database, {});
  auto active = active_session(*proxy);
  answer_session(*database, active);
  EXPECT_FALSE(proxy->HasClient());
  EXPECT_TRUE(DecodeSentMessages<baseapp::AuthenticateResult>(*client).empty());
  const auto requests = DecodeSentMessages<dbapp::AccountSessionRequest>(*database);
  ASSERT_TRUE(requests.back().operation);
  EXPECT_EQ(requests.back().operation->action, AccountSessionAction::kIssueRecovery);
  auto issued = apply_latest(*database, active);
  answer_session(*database, issued);
  ASSERT_TRUE(proxy->HasClient());
  const auto replies = DecodeSentMessages<baseapp::AuthenticateResult>(*client);
  ASSERT_EQ(replies.size(), 1u);
  ASSERT_TRUE(replies.back().success);
  EXPECT_EQ(replies.back().recovery.account_dbid, 42);
  EXPECT_EQ(replies.back().recovery.expires_at_ms, 601000);
  const auto hash = Sha256(replies.back().recovery.value);
  ASSERT_TRUE(hash.HasValue());
  EXPECT_EQ(*hash, issued.recovery.credential_hash);
  EXPECT_EQ(session_pending_count(), 0u);
}

TEST_F(BaseAppRollbackTest, LiveRecoveryConsumesOnceBeforeReplacingTheBoundConnection) {
  InterfaceTable table;
  auto database = make_intrusive<CapturingChannel>(dispatcher_, table, Address("127.0.0.1", 24002));
  auto client = make_intrusive<CapturingChannel>(dispatcher_, table, Address("127.0.0.1", 27001));
  set_legacy_dbapp_channel(database.get());
  auto* proxy = prepare_auth_proxy();
  ASSERT_NE(proxy, nullptr);
  authenticate(*client, proxy->GetSessionKey(), true, true);
  answer_session(*database, {});
  auto active = active_session(*proxy);
  answer_session(*database, active);
  auto issued = apply_latest(*database, active);
  answer_session(*database, issued);
  ASSERT_TRUE(proxy->HasClient());
  const auto old_key = proxy->GetSessionKey();
  auto request = resume_request(issued);
  auto result = std::make_shared<std::optional<Result<login::PrepareResumeResult>>>();
  prepare_resume(request, result);
  answer_session(*database, issued);
  EXPECT_TRUE(proxy->HasClient());
  EXPECT_TRUE(session_frozen(proxy->EntityId()));
  EXPECT_FALSE(result->has_value());
  auto consumed = apply_latest(*database, issued);
  EXPECT_NE(consumed.recovery.credential_hash, issued.recovery.credential_hash);
  answer_session(*database, consumed);
  ASSERT_TRUE(result->has_value());
  ASSERT_TRUE(**result);
  EXPECT_TRUE((**result)->success);
  EXPECT_EQ((**result)->entity_id, proxy->EntityId());
  EXPECT_FALSE(proxy->HasClient());
  EXPECT_NE(proxy->GetSessionKey(), old_key);
  EXPECT_EQ(proxy->GetSessionKey(), request.session_key);
  auto replacement =
      make_intrusive<CapturingChannel>(dispatcher_, table, Address("127.0.0.1", 27002));
  authenticate(*replacement, request.session_key, true, true);
  EXPECT_TRUE(proxy->HasClient());
  auto replies = DecodeSentMessages<baseapp::AuthenticateResult>(*replacement);
  ASSERT_EQ(replies.size(), 1u);
  EXPECT_TRUE(replies.front().success);
  auto digest = Sha256(replies.front().recovery.value);
  ASSERT_TRUE(digest);
  EXPECT_EQ(*digest, consumed.recovery.credential_hash);
  result->reset();
  prepare_resume(request, result);
  answer_session(*database, consumed);
  ASSERT_TRUE(result->has_value());
  EXPECT_FALSE(**result);
  EXPECT_TRUE(proxy->HasClient());
  EXPECT_FALSE(session_frozen(proxy->EntityId()));
}

TEST_F(BaseAppRollbackTest, RecoveryCommitUnknownKeepsOldConnectionFencedAndWithholdsSuccess) {
  InterfaceTable table;
  auto database = make_intrusive<CapturingChannel>(dispatcher_, table, Address("127.0.0.1", 24002));
  auto client = make_intrusive<CapturingChannel>(dispatcher_, table, Address("127.0.0.1", 27001));
  set_legacy_dbapp_channel(database.get());
  auto* proxy = prepare_auth_proxy();
  authenticate(*client, proxy->GetSessionKey(), true, true);
  answer_session(*database, {});
  auto active = active_session(*proxy);
  answer_session(*database, active);
  auto issued = apply_latest(*database, active);
  answer_session(*database, issued);
  const auto old_key = proxy->GetSessionKey();
  auto request = resume_request(issued);
  auto result = std::make_shared<std::optional<Result<login::PrepareResumeResult>>>();
  prepare_resume(request, result);
  answer_session(*database, issued);
  answer_session(*database, issued, ErrorCode::kCommitUnknown);
  ASSERT_TRUE(result->has_value());
  EXPECT_FALSE(**result);
  EXPECT_EQ((**result).Error().Code(), ErrorCode::kCommitUnknown);
  EXPECT_EQ(proxy->GetSessionKey(), old_key);
  EXPECT_TRUE(session_frozen(proxy->EntityId()));
}

TEST_F(BaseAppRollbackTest, CrashRecoveryRestoresPersistentIdentityAndWaitsForOwnershipCommit) {
  prepare_crash_recovery();
  InterfaceTable table;
  auto database = make_intrusive<CapturingChannel>(dispatcher_, table, Address("127.0.0.1", 24002));
  set_legacy_dbapp_channel(database.get());
  AccountSession old;
  old.version = 3;
  old.phase = AccountSessionPhase::kActive;
  old.owner = {Address("127.0.0.1", 25002), 999, 222};
  old.recovery = {{}, 42, 7, 601000};
  old.recovery.credential_hash[0] = std::byte{1};
  auto request = resume_request(old);
  retire_base_peer(old.owner.base_addr, 222);
  auto result = std::make_shared<std::optional<Result<login::PrepareResumeResult>>>();
  prepare_resume(request, result);
  answer_session(*database, old);
  EXPECT_FALSE(result->has_value());
  answer_recovery_checkout(*database);
  auto* entity = find_db_entity(42);
  ASSERT_NE(entity, nullptr);
  EXPECT_NE(entity->EntityId(), old.owner.entity_id);
  EXPECT_EQ(entity->EntityData(), (std::vector<std::byte>{std::byte{0x11}}));
  EXPECT_EQ(entity->CellBackupData(), (std::vector<std::byte>{std::byte{0x22}}));
  EXPECT_TRUE(session_frozen(entity->EntityId()));
  auto mutations = DecodeSentMessages<dbapp::AccountSessionRequest>(*database);
  ASSERT_TRUE(mutations.back().operation);
  const auto& operation = *mutations.back().operation;
  EXPECT_EQ(operation.action, AccountSessionAction::kClaimRecovery);
  EXPECT_EQ(operation.claim_checkout, entity->CheckoutOwner());
  EXPECT_EQ(operation.recovery_checkout, entity->CheckoutOwner());
  EXPECT_FALSE(result->has_value());
  auto claimed = apply_latest(*database, old);
  answer_session(*database, claimed);
  ASSERT_TRUE(result->has_value());
  ASSERT_TRUE(**result);
  EXPECT_TRUE((**result)->success);
  EXPECT_EQ((**result)->entity_id, entity->EntityId());
  auto client = make_intrusive<CapturingChannel>(dispatcher_, table, Address("127.0.0.1", 27001));
  authenticate(*client, request.session_key, true, true);
  const auto replies = DecodeSentMessages<baseapp::AuthenticateResult>(*client);
  ASSERT_EQ(replies.size(), 1u);
  EXPECT_TRUE(replies.front().success);
}

TEST_F(BaseAppRollbackTest, LogoutRejectsForeignChannelAndWaitsForConfirmedRevocation) {
  InterfaceTable table;
  auto database = make_intrusive<CapturingChannel>(dispatcher_, table, Address("127.0.0.1", 24002));
  auto foreign = make_intrusive<CapturingChannel>(dispatcher_, table, Address("127.0.0.1", 27002));
  set_legacy_dbapp_channel(database.get());
  auto* client = retained_client_channel(Address("127.0.0.1", 27001));
  ASSERT_NE(client, nullptr);
  auto* proxy = prepare_auth_proxy();
  const auto entity_id = proxy->EntityId();
  authenticate(*client, proxy->GetSessionKey(), true, true);
  answer_session(*database, {});
  auto active = active_session(*proxy);
  answer_session(*database, active);
  auto issued = apply_latest(*database, active);
  answer_session(*database, issued);
  const auto before = database->send_count();
  logout(*foreign, entity_id, 91);
  EXPECT_EQ(database->send_count(), before);
  auto rejected = DecodeSentMessages<baseapp::ClientLogoutResult>(*foreign);
  ASSERT_EQ(rejected.size(), 1u);
  EXPECT_EQ(rejected.front().status, ErrorCode::kPermissionDenied);
  EXPECT_TRUE(proxy->HasClient());
  logout(*client, entity_id, 92);
  EXPECT_TRUE(session_frozen(entity_id));
  EXPECT_TRUE(proxy->HasClient());
  answer_session(*database, issued);
  auto released = apply_latest(*database, issued);
  EXPECT_EQ(released.phase, AccountSessionPhase::kOffline);
  EXPECT_EQ(released.recovery, AccountRecoveryGrant{});
  answer_session(*database, released);
  EXPECT_EQ(session_pending_count(), 0u);
  EXPECT_FALSE(proxy->HasClient());
}

TEST_F(BaseAppRollbackTest, CrashRecoveryRejectsTemporaryEntitiesBeforeCheckout) {
  prepare_crash_recovery();
  InterfaceTable table;
  auto database = make_intrusive<CapturingChannel>(dispatcher_, table, Address("127.0.0.1", 24002));
  set_legacy_dbapp_channel(database.get());
  AccountSession old;
  old.version = 3;
  old.phase = AccountSessionPhase::kActive;
  old.owner = {Address("127.0.0.1", 25002), 999, 222};
  old.recovery = {{}, 0, 7, 601000};
  old.recovery.credential_hash[0] = std::byte{1};
  auto request = resume_request(old);
  retire_base_peer(old.owner.base_addr, 222);
  auto result = std::make_shared<std::optional<Result<login::PrepareResumeResult>>>();
  prepare_resume(request, result);
  answer_session(*database, old);
  ASSERT_TRUE(result->has_value());
  EXPECT_FALSE(**result);
  EXPECT_TRUE(DecodeSentMessages<dbapp::CheckoutEntity>(*database).empty());
}

TEST_F(BaseAppRollbackTest, PersistedRecoveryIdentityChangesOnlyAfterConfirmedGrant) {
  for (const bool fail : {false, true}) {
    InterfaceTable table;
    auto database =
        make_intrusive<CapturingChannel>(dispatcher_, table, Address("127.0.0.1", 24002));
    auto client = make_intrusive<CapturingChannel>(dispatcher_, table,
                                                   Address("127.0.0.1", fail ? 27002 : 27001));
    set_legacy_dbapp_channel(database.get());
    auto* proxy = prepare_auth_proxy(fail ? 43 : 42);
    const auto entity_id = proxy->EntityId();
    authenticate(*client, proxy->GetSessionKey(), true, true);
    answer_session(*database, {});
    auto active = active_session(*proxy);
    answer_session(*database, active);
    auto issued = apply_latest(*database, active);
    answer_session(*database, issued);
    const auto old_dbid = proxy->Dbid();
    refresh_recovery_after_save(entity_id, fail ? 85 : 84);
    EXPECT_EQ(recovery_target(entity_id), old_dbid);
    auto requests = DecodeSentMessages<dbapp::AccountSessionRequest>(*database);
    ASSERT_TRUE(requests.back().operation);
    EXPECT_EQ(requests.back().operation->action, AccountSessionAction::kIssueRecovery);
    EXPECT_EQ(requests.back().operation->recovery.entity_dbid, proxy->Dbid());
    if (fail) {
      answer_session(*database, issued, ErrorCode::kCommitUnknown);
      EXPECT_EQ(recovery_target(entity_id), old_dbid);
    } else {
      auto refreshed = apply_latest(*database, issued);
      answer_session(*database, refreshed);
      EXPECT_EQ(recovery_target(entity_id), proxy->Dbid());
      const auto sent = database->send_count();
      refresh_recovery_after_save(entity_id, proxy->Dbid());
      EXPECT_EQ(database->send_count(), sent);
    }
  }
}

TEST_F(BaseAppRollbackTest, UnknownRecoveryGrantCommitCannotExposeCredentialsOrBind) {
  InterfaceTable table;
  auto database = make_intrusive<CapturingChannel>(dispatcher_, table, Address("127.0.0.1", 24002));
  auto client = make_intrusive<CapturingChannel>(dispatcher_, table, Address("127.0.0.1", 27001));
  set_legacy_dbapp_channel(database.get());
  auto* proxy = prepare_auth_proxy();
  ASSERT_NE(proxy, nullptr);
  authenticate(*client, proxy->GetSessionKey(), true, true);
  answer_session(*database, {});
  auto active = active_session(*proxy);
  answer_session(*database, active);
  answer_session(*database, active, ErrorCode::kCommitUnknown);
  EXPECT_FALSE(proxy->HasClient());
  EXPECT_TRUE(session_frozen(proxy->EntityId()));
  const auto replies = DecodeSentMessages<baseapp::AuthenticateResult>(*client);
  ASSERT_EQ(replies.size(), 1u);
  EXPECT_FALSE(replies.back().success);
  EXPECT_FALSE(replies.back().recovery.IsPresent());
}

TEST_F(BaseAppRollbackTest, AuthenticationWaitsForConfirmedSessionGrant) {
  InterfaceTable table;
  auto database = make_intrusive<CapturingChannel>(dispatcher_, table, Address("127.0.0.1", 24002));
  auto client = make_intrusive<CapturingChannel>(dispatcher_, table, Address("127.0.0.1", 27001));
  set_legacy_dbapp_channel(database.get());
  auto* proxy = prepare_auth_proxy();
  ASSERT_NE(proxy, nullptr);
  authenticate(*client, proxy->GetSessionKey());
  EXPECT_FALSE(proxy->HasClient());
  EXPECT_EQ(client->send_count(), 0u);
  answer_session(*database, AccountSession{});
  auto requests = DecodeSentMessages<dbapp::AccountSessionRequest>(*database);
  ASSERT_EQ(requests.size(), 2u);
  ASSERT_TRUE(requests.back().operation.has_value());
  EXPECT_EQ(requests.back().operation->action, AccountSessionAction::kClaim);
  EXPECT_EQ(requests.back().operation->claim_checkout, proxy->CheckoutOwner());
  EXPECT_FALSE(proxy->HasClient());
  const auto grant = active_session(*proxy);
  answer_session(*database, grant);
  EXPECT_TRUE(proxy->HasClient());
  EXPECT_FALSE(session_frozen(proxy->EntityId()));
  const auto replies = DecodeSentMessages<baseapp::AuthenticateResult>(*client);
  ASSERT_EQ(replies.size(), 1u);
  EXPECT_TRUE(replies[0].success);
}

TEST_F(BaseAppRollbackTest, AuthenticationCommitUnknownReusesTheOriginalOperation) {
  InterfaceTable table;
  auto database = make_intrusive<CapturingChannel>(dispatcher_, table, Address("127.0.0.1", 24002));
  auto client = make_intrusive<CapturingChannel>(dispatcher_, table, Address("127.0.0.1", 27001));
  set_legacy_dbapp_channel(database.get());
  auto* proxy = prepare_auth_proxy();
  ASSERT_NE(proxy, nullptr);
  authenticate(*client, proxy->GetSessionKey());
  answer_session(*database, AccountSession{});
  auto issued = DecodeSentMessages<dbapp::AccountSessionRequest>(*database).back();
  ASSERT_TRUE(issued.operation.has_value());
  answer_session(*database, AccountSession{}, ErrorCode::kCommitUnknown);
  EXPECT_FALSE(proxy->HasClient());
  EXPECT_TRUE(session_frozen(proxy->EntityId()));
  authenticate(*client, proxy->GetSessionKey());
  const auto repeated = DecodeSentMessages<dbapp::AccountSessionRequest>(*database).back();
  ASSERT_TRUE(repeated.operation.has_value());
  EXPECT_EQ(repeated.operation->id, issued.operation->id);
  EXPECT_NE(repeated.request_id, issued.request_id);
  answer_session(*database, active_session(*proxy));
  EXPECT_TRUE(proxy->HasClient());
  EXPECT_EQ(session_pending_count(), 0u);
}

TEST_F(BaseAppRollbackTest, HandoffRollbackCannotReactivateADestroyingSource) {
  InterfaceTable table;
  auto database = make_intrusive<CapturingChannel>(dispatcher_, table, Address("127.0.0.1", 24002));
  auto client = make_intrusive<CapturingChannel>(dispatcher_, table, Address("127.0.0.1", 27001));
  set_legacy_dbapp_channel(database.get());
  auto* proxy = prepare_auth_proxy();
  ASSERT_NE(proxy, nullptr);
  authenticate(*client, proxy->GetSessionKey());
  answer_session(*database, {});
  const auto active = active_session(*proxy);
  answer_session(*database, active);
  auto result = std::make_shared<std::optional<Result<void>>>();
  recover_remote_session(proxy->EntityId(), result);
  proxy->MarkForDestroy();
  answer_session(*database, active);
  ASSERT_TRUE(result->has_value());
  ASSERT_FALSE((*result)->HasValue());
  EXPECT_EQ((*result)->Error().Code(), ErrorCode::kCommitUnknown);
  EXPECT_TRUE(session_frozen(proxy->EntityId()));
}

TEST_F(BaseAppRollbackTest, OverloadedReplayDoesNotForgetAnUnconfirmedOperation) {
  InterfaceTable table;
  auto database = make_intrusive<CapturingChannel>(dispatcher_, table, Address("127.0.0.1", 24002));
  auto client = make_intrusive<CapturingChannel>(dispatcher_, table, Address("127.0.0.1", 27001));
  set_legacy_dbapp_channel(database.get());
  auto* proxy = prepare_auth_proxy();
  ASSERT_NE(proxy, nullptr);
  authenticate(*client, proxy->GetSessionKey());
  answer_session(*database, {});
  const auto original = DecodeSentMessages<dbapp::AccountSessionRequest>(*database).back();
  ASSERT_TRUE(original.operation.has_value());
  answer_session(*database, {}, ErrorCode::kCommitUnknown);
  for (const auto status : {ErrorCode::kRateLimited, ErrorCode::kWouldBlock}) {
    authenticate(*client, proxy->GetSessionKey());
    auto retry = DecodeSentMessages<dbapp::AccountSessionRequest>(*database).back();
    ASSERT_TRUE(retry.operation.has_value());
    EXPECT_EQ(retry.operation->id, original.operation->id);
    answer_session(*database, {}, status);
    EXPECT_FALSE(proxy->HasClient());
    EXPECT_TRUE(session_frozen(proxy->EntityId()));
  }
  authenticate(*client, proxy->GetSessionKey());
  const auto retry = DecodeSentMessages<dbapp::AccountSessionRequest>(*database).back();
  ASSERT_TRUE(retry.operation.has_value());
  EXPECT_EQ(retry.operation->id, original.operation->id);
  answer_session(*database, active_session(*proxy));
  EXPECT_TRUE(proxy->HasClient());
}

TEST_F(BaseAppRollbackTest, SupersededReplayRequiresAMonotonicDatabaseConfirmation) {
  InterfaceTable table;
  auto database = make_intrusive<CapturingChannel>(dispatcher_, table, Address("127.0.0.1", 24002));
  auto client = make_intrusive<CapturingChannel>(dispatcher_, table, Address("127.0.0.1", 27001));
  set_legacy_dbapp_channel(database.get());
  auto* proxy = prepare_auth_proxy();
  ASSERT_NE(proxy, nullptr);
  authenticate(*client, proxy->GetSessionKey());
  answer_session(*database, {});
  const auto original = DecodeSentMessages<dbapp::AccountSessionRequest>(*database).back();
  answer_session(*database, {}, ErrorCode::kCommitUnknown);
  authenticate(*client, proxy->GetSessionKey());
  answer_session(*database, {}, ErrorCode::kPermissionDenied);
  EXPECT_FALSE(DecodeSentMessages<dbapp::AccountSessionRequest>(*database).back().operation);
  answer_session(*database, {});
  authenticate(*client, proxy->GetSessionKey());
  const auto retry = DecodeSentMessages<dbapp::AccountSessionRequest>(*database).back();
  ASSERT_TRUE(retry.operation.has_value());
  ASSERT_TRUE(original.operation.has_value());
  EXPECT_EQ(retry.operation->id, original.operation->id);
  answer_session(*database, {}, ErrorCode::kPermissionDenied);
  AccountSession advanced;
  advanced.version = 2;
  answer_session(*database, advanced);
  EXPECT_FALSE(proxy->HasClient());
  authenticate(*client, proxy->GetSessionKey());
  EXPECT_FALSE(DecodeSentMessages<dbapp::AccountSessionRequest>(*database).back().operation);
  answer_session(*database, advanced);
  const auto fresh = DecodeSentMessages<dbapp::AccountSessionRequest>(*database).back();
  ASSERT_TRUE(fresh.operation.has_value());
  EXPECT_NE(fresh.operation->id, original.operation->id);
  EXPECT_EQ(fresh.operation->expected_version, 2u);
  answer_session(*database, active_session(*proxy, 3));
  EXPECT_TRUE(proxy->HasClient());
}

TEST_F(BaseAppRollbackTest, OverloadedRevocationCannotDropAnUnconfirmedClaim) {
  InterfaceTable table;
  auto database = make_intrusive<CapturingChannel>(dispatcher_, table, Address("127.0.0.1", 24002));
  auto client = make_intrusive<CapturingChannel>(dispatcher_, table, Address("127.0.0.1", 27001));
  set_legacy_dbapp_channel(database.get());
  auto* proxy = prepare_auth_proxy();
  ASSERT_NE(proxy, nullptr);
  authenticate(*client, proxy->GetSessionKey());
  answer_session(*database, {});
  answer_session(*database, {}, ErrorCode::kCommitUnknown);
  auto released = std::make_shared<std::optional<Result<void>>>();
  release_session(proxy->EntityId(), released);
  answer_session(*database, {}, ErrorCode::kRateLimited);
  ASSERT_TRUE(released->has_value());
  ASSERT_FALSE((*released)->HasValue());
  EXPECT_EQ((*released)->Error().Code(), ErrorCode::kCommitUnknown);
  EXPECT_FALSE(proxy->GetSessionKey().IsZero());
  EXPECT_TRUE(session_frozen(proxy->EntityId()));
  released->reset();
  release_session(proxy->EntityId(), released);
  const auto active = active_session(*proxy);
  answer_session(*database, active);
  answer_session(*database, active);
  const auto offline = apply_latest(*database, active);
  answer_session(*database, offline);
  ASSERT_TRUE(released->has_value());
  EXPECT_TRUE((*released)->HasValue());
  EXPECT_TRUE(proxy->GetSessionKey().IsZero());
}

TEST_F(BaseAppRollbackTest, AuthenticationRejectsForeignOwnershipAndUnpreparedKeys) {
  InterfaceTable table;
  auto database = make_intrusive<CapturingChannel>(dispatcher_, table, Address("127.0.0.1", 24002));
  auto client = make_intrusive<CapturingChannel>(dispatcher_, table, Address("127.0.0.1", 27001));
  set_legacy_dbapp_channel(database.get());
  auto* proxy = prepare_auth_proxy();
  ASSERT_NE(proxy, nullptr);
  authenticate(*client, proxy->GetSessionKey());
  auto foreign = active_session(*proxy);
  foreign.owner.process_instance = 222;
  answer_session(*database, foreign);
  EXPECT_FALSE(proxy->HasClient());
  EXPECT_EQ(DecodeSentMessages<dbapp::AccountSessionRequest>(*database).size(), 1u);
  const auto replies = DecodeSentMessages<baseapp::AuthenticateResult>(*client);
  ASSERT_EQ(replies.size(), 1u);
  EXPECT_FALSE(replies[0].success);
  auto* unprepared = new_proxy();
  ASSERT_NE(unprepared, nullptr);
  auto key = SessionKey::Generate();
  ASSERT_TRUE(key.HasValue());
  ASSERT_TRUE(app_.GetEntityManager().AssignSessionKey(unprepared->EntityId(), *key));
  authenticate(*client, *key);
  EXPECT_FALSE(unprepared->HasClient());
}

TEST_F(BaseAppRollbackTest, CancelAuthenticationResolvesGrantBeforeReleasingCheckout) {
  InterfaceTable table;
  auto database = make_intrusive<CapturingChannel>(dispatcher_, table, Address("127.0.0.1", 24002));
  auto client = make_intrusive<CapturingChannel>(dispatcher_, table, Address("127.0.0.1", 27001));
  set_legacy_dbapp_channel(database.get());
  auto* proxy = prepare_auth_proxy();
  ASSERT_NE(proxy, nullptr);
  authenticate(*client, proxy->GetSessionKey());
  answer_session(*database, AccountSession{});
  auto issued = DecodeSentMessages<dbapp::AccountSessionRequest>(*database).back();
  ASSERT_TRUE(rollback_auth_proxy(*proxy));
  EXPECT_FALSE(proxy->IsPendingDestroy());
  EXPECT_TRUE(DecodeSentMessages<dbapp::CheckinEntity>(*database).empty());
  const auto repeated = DecodeSentMessages<dbapp::AccountSessionRequest>(*database).back();
  ASSERT_TRUE(repeated.operation.has_value());
  EXPECT_EQ(repeated.operation->id, issued.operation->id);
  auto grant = active_session(*proxy);
  answer_session(*database, grant);
  answer_session(*database, grant);
  auto releasing = DecodeSentMessages<dbapp::AccountSessionRequest>(*database).back();
  ASSERT_TRUE(releasing.operation.has_value());
  EXPECT_EQ(releasing.operation->action, AccountSessionAction::kRelease);
  EXPECT_FALSE(proxy->HasClient());
  EXPECT_FALSE(proxy->IsPendingDestroy());
  AccountSession offline;
  offline.version = 2;
  answer_session(*database, offline);
  EXPECT_TRUE(proxy->IsPendingDestroy());
  EXPECT_FALSE(DecodeSentMessages<dbapp::CheckinEntity>(*database).empty());
  EXPECT_EQ(session_pending_count(), 0u);
}

TEST_F(BaseAppRollbackTest, LocalClientMoveWaitsForCommitAndReservesItsTarget) {
  InterfaceTable table;
  auto database = make_intrusive<CapturingChannel>(dispatcher_, table, Address("127.0.0.1", 24002));
  auto client = make_intrusive<CapturingChannel>(dispatcher_, table, Address("127.0.0.1", 27001));
  set_legacy_dbapp_channel(database.get());
  retain_client_channel(client->RemoteAddress());
  auto* source = prepare_auth_proxy();
  auto* target = new_proxy();
  ASSERT_NE(source, nullptr);
  ASSERT_NE(target, nullptr);
  authenticate(*client, source->GetSessionKey());
  answer_session(*database, AccountSession{});
  auto grant = active_session(*source);
  answer_session(*database, grant);
  auto result = std::make_shared<std::optional<Result<void>>>();
  move_session(source->EntityId(), target->EntityId(), result);
  EXPECT_TRUE(source->HasClient());
  EXPECT_FALSE(target->HasClient());
  EXPECT_FALSE(result->has_value());
  EXPECT_TRUE(session_frozen(source->EntityId()));
  auto again = std::make_shared<std::optional<Result<void>>>();
  move_session(source->EntityId(), target->EntityId(), again);
  ASSERT_TRUE(again->has_value());
  EXPECT_FALSE(again->value().HasValue());
  const auto issued = DecodeSentMessages<dbapp::AccountSessionRequest>(*database).back();
  ASSERT_TRUE(issued.operation.has_value());
  EXPECT_EQ(issued.operation->action, AccountSessionAction::kMoveLocal);
  grant.version = 2;
  grant.owner.entity_id = target->EntityId();
  answer_session(*database, grant);
  ASSERT_TRUE(result->has_value());
  EXPECT_TRUE(result->value().HasValue());
  EXPECT_FALSE(source->HasClient());
  EXPECT_TRUE(target->HasClient());
  EXPECT_FALSE(session_frozen(target->EntityId()));
}

TEST_F(BaseAppRollbackTest, LocalClientMoveCommitUnknownRetainsTheSourceBinding) {
  InterfaceTable table;
  auto database = make_intrusive<CapturingChannel>(dispatcher_, table, Address("127.0.0.1", 24002));
  auto client = make_intrusive<CapturingChannel>(dispatcher_, table, Address("127.0.0.1", 27001));
  set_legacy_dbapp_channel(database.get());
  auto* source = prepare_auth_proxy();
  auto* target = new_proxy();
  ASSERT_NE(source, nullptr);
  ASSERT_NE(target, nullptr);
  authenticate(*client, source->GetSessionKey());
  answer_session(*database, AccountSession{});
  auto grant = active_session(*source);
  answer_session(*database, grant);
  auto result = std::make_shared<std::optional<Result<void>>>();
  move_session(source->EntityId(), target->EntityId(), result);
  auto issued = DecodeSentMessages<dbapp::AccountSessionRequest>(*database).back();
  answer_session(*database, {}, ErrorCode::kCommitUnknown);
  ASSERT_TRUE(result->has_value());
  EXPECT_EQ(result->value().Error().Code(), ErrorCode::kCommitUnknown);
  EXPECT_TRUE(source->HasClient());
  EXPECT_FALSE(target->HasClient());
  EXPECT_TRUE(session_frozen(source->EntityId()));
  auto released = std::make_shared<std::optional<Result<void>>>();
  release_session(source->EntityId(), released);
  const auto repeated = DecodeSentMessages<dbapp::AccountSessionRequest>(*database).back();
  ASSERT_TRUE(repeated.operation.has_value());
  EXPECT_EQ(repeated.operation->id, issued.operation->id);
  grant.version = 2;
  grant.owner.entity_id = target->EntityId();
  answer_session(*database, grant);
  answer_session(*database, grant);
  auto releasing = DecodeSentMessages<dbapp::AccountSessionRequest>(*database).back();
  ASSERT_TRUE(releasing.operation.has_value());
  EXPECT_EQ(releasing.operation->actor.entity_id, target->EntityId());
  AccountSession offline;
  offline.version = 3;
  answer_session(*database, offline);
  ASSERT_TRUE(released->has_value());
  EXPECT_TRUE(released->value().HasValue());
  EXPECT_FALSE(source->HasClient());
  EXPECT_FALSE(target->HasClient());
}

TEST_F(BaseAppRollbackTest, FailedClientMoveUnfreezesOnlyAfterConfirmingOriginalOwnership) {
  InterfaceTable table;
  auto database = make_intrusive<CapturingChannel>(dispatcher_, table, Address("127.0.0.1", 24002));
  auto client = make_intrusive<CapturingChannel>(dispatcher_, table, Address("127.0.0.1", 27001));
  set_legacy_dbapp_channel(database.get());
  auto* source = prepare_auth_proxy();
  auto* target = new_proxy();
  ASSERT_NE(source, nullptr);
  ASSERT_NE(target, nullptr);
  authenticate(*client, source->GetSessionKey());
  answer_session(*database, {});
  const auto grant = active_session(*source);
  answer_session(*database, grant);
  auto result = std::make_shared<std::optional<Result<void>>>();
  move_session(source->EntityId(), target->EntityId(), result);
  answer_session(*database, {}, ErrorCode::kPermissionDenied);
  EXPECT_TRUE(session_frozen(source->EntityId()));
  EXPECT_FALSE(result->has_value());
  answer_session(*database, grant);
  ASSERT_TRUE(result->has_value());
  EXPECT_FALSE(result->value().HasValue());
  EXPECT_FALSE(session_frozen(source->EntityId()));
  EXPECT_TRUE(source->HasClient());
  EXPECT_FALSE(target->HasClient());
}

TEST_F(BaseAppRollbackTest, PendingClientMovePreventsDestinationTeardown) {
  InterfaceTable table;
  auto database = make_intrusive<CapturingChannel>(dispatcher_, table, Address("127.0.0.1", 24002));
  auto client = make_intrusive<CapturingChannel>(dispatcher_, table, Address("127.0.0.1", 27001));
  set_legacy_dbapp_channel(database.get());
  auto* source = prepare_auth_proxy();
  auto* target = new_proxy();
  ASSERT_NE(source, nullptr);
  ASSERT_NE(target, nullptr);
  authenticate(*client, source->GetSessionKey());
  answer_session(*database, {});
  auto grant = active_session(*source);
  answer_session(*database, grant);
  auto result = std::make_shared<std::optional<Result<void>>>();
  move_session(source->EntityId(), target->EntityId(), result);
  EXPECT_TRUE(transfer_target_reserved(target->EntityId()));
  EXPECT_FALSE(finalize_proxy(target->EntityId()));
  EXPECT_FALSE(target->IsPendingDestroy());
  target->MarkForDestroy();
  grant.version = 2;
  grant.owner.entity_id = target->EntityId();
  answer_session(*database, grant);
  ASSERT_TRUE(result->has_value());
  ASSERT_FALSE(result->value().HasValue());
  EXPECT_EQ(result->value().Error().Code(), ErrorCode::kCommitUnknown);
  EXPECT_TRUE(source->HasClient());
  EXPECT_FALSE(target->HasClient());
  EXPECT_TRUE(session_frozen(source->EntityId()));
}

TEST_F(BaseAppRollbackTest, MissingClientChannelAfterMoveCannotReportSuccess) {
  InterfaceTable table;
  auto database = make_intrusive<CapturingChannel>(dispatcher_, table, Address("127.0.0.1", 24002));
  auto client = make_intrusive<CapturingChannel>(dispatcher_, table, Address("127.0.0.1", 27001));
  set_legacy_dbapp_channel(database.get());
  auto* source = prepare_auth_proxy();
  auto* target = new_proxy();
  ASSERT_NE(source, nullptr);
  ASSERT_NE(target, nullptr);
  authenticate(*client, source->GetSessionKey());
  answer_session(*database, {});
  auto grant = active_session(*source);
  answer_session(*database, grant);
  auto result = std::make_shared<std::optional<Result<void>>>();
  move_session(source->EntityId(), target->EntityId(), result);
  grant.version = 2;
  grant.owner.entity_id = target->EntityId();
  answer_session(*database, grant);
  ASSERT_TRUE(result->has_value());
  ASSERT_FALSE(result->value().HasValue());
  EXPECT_EQ(result->value().Error().Code(), ErrorCode::kCommitUnknown);
  EXPECT_FALSE(source->HasClient());
  EXPECT_FALSE(target->HasClient());
}

TEST_F(BaseAppRollbackTest, AccountSessionQueryMatchesChannelProcessAndRequest) {
  InterfaceTable table;
  auto channel = make_intrusive<CapturingChannel>(dispatcher_, table, Address("127.0.0.1", 24002));
  auto wrong = make_intrusive<CapturingChannel>(dispatcher_, table, Address("127.0.0.1", 24003));
  set_legacy_dbapp_channel(channel.get());
  auto result = std::make_shared<std::optional<Result<AccountSession>>>();
  query_session(result);
  const auto requests = DecodeSentMessages<dbapp::AccountSessionRequest>(*channel);
  ASSERT_EQ(requests.size(), 1u);
  EXPECT_EQ(requests[0].account_dbid, 42);
  EXPECT_EQ(requests[0].process_instance, 111u);
  EXPECT_FALSE(result->has_value());
  dbapp::AccountSessionReply reply;
  reply.request_id = requests[0].request_id;
  reply.requester_instance = 222;
  reply.account_dbid = 42;
  reply.account_type_id = 7;
  session_reply(channel.get(), reply);
  reply.requester_instance = 111;
  session_reply(wrong.get(), reply);
  session_reply(nullptr, reply);
  EXPECT_FALSE(result->has_value());
  EXPECT_EQ(session_pending_count(), 1u);
  reply.session.version = 7;
  session_reply(channel.get(), reply);
  ASSERT_TRUE(result->has_value());
  ASSERT_TRUE(result->value().HasValue());
  EXPECT_EQ(result->value()->version, 7u);
  EXPECT_EQ(session_pending_count(), 0u);

  auto next = std::make_shared<std::optional<Result<AccountSession>>>();
  query_session(next);
  session_reply(channel.get(), reply);
  EXPECT_FALSE(next->has_value());
  const auto updated = DecodeSentMessages<dbapp::AccountSessionRequest>(*channel);
  ASSERT_EQ(updated.size(), 2u);
  EXPECT_NE(updated[1].request_id, reply.request_id);
  reply.request_id = updated[1].request_id;
  reply.status = ErrorCode::kIoError;
  reply.error = "injected read failure";
  session_reply(channel.get(), reply);
  ASSERT_TRUE(next->has_value());
  ASSERT_FALSE(next->value().HasValue());
  EXPECT_EQ(next->value().Error().Code(), ErrorCode::kIoError);
}

TEST_F(BaseAppRollbackTest, AccountSessionMutationCancellationCannotClaimRollback) {
  InterfaceTable table;
  auto channel = make_intrusive<CapturingChannel>(dispatcher_, table, Address("127.0.0.1", 24002));
  set_legacy_dbapp_channel(channel.get());
  AccountSessionOperation operation;
  operation.id[0] = std::byte{1};
  operation.account_dbid = 42;
  operation.account_type_id = 7;
  operation.action = AccountSessionAction::kRelease;
  operation.actor = {Address("127.0.0.1", 25001), 9, 111};
  auto result = std::make_shared<std::optional<Result<AccountSession>>>();
  CancellationSource cancel;
  query_session(result, operation, cancel.Token());
  EXPECT_EQ(session_pending_count(), 1u);
  cancel.RequestCancellation();
  ASSERT_TRUE(result->has_value());
  ASSERT_FALSE(result->value().HasValue());
  EXPECT_EQ(result->value().Error().Code(), ErrorCode::kCommitUnknown);
  EXPECT_EQ(session_pending_count(), 0u);
}

TEST_F(BaseAppRollbackTest, AccountSessionRejectsMismatchedReplyMetadata) {
  InterfaceTable table;
  auto channel = make_intrusive<CapturingChannel>(dispatcher_, table, Address("127.0.0.1", 24002));
  set_legacy_dbapp_channel(channel.get());
  auto result = std::make_shared<std::optional<Result<AccountSession>>>();
  query_session(result);
  const auto requests = DecodeSentMessages<dbapp::AccountSessionRequest>(*channel);
  ASSERT_EQ(requests.size(), 1u);
  dbapp::AccountSessionReply reply;
  reply.request_id = requests[0].request_id;
  reply.requester_instance = 111;
  reply.account_dbid = 43;
  reply.account_type_id = 7;
  session_reply(channel.get(), reply);
  ASSERT_TRUE(result->has_value());
  ASSERT_FALSE(result->value().HasValue());
  EXPECT_EQ(result->value().Error().Code(), ErrorCode::kInvalidArgument);
  EXPECT_EQ(session_pending_count(), 0u);
}

TEST_F(BaseAppRollbackTest, AccountSessionDisconnectDoesNotLeaveAWaitOrInventAnOfflineSession) {
  InterfaceTable table;
  auto channel = make_intrusive<CapturingChannel>(dispatcher_, table, Address("127.0.0.1", 24002));
  set_legacy_dbapp_channel(channel.get());
  auto result = std::make_shared<std::optional<Result<AccountSession>>>();
  query_session(result);
  EXPECT_EQ(session_pending_count(), 1u);
  disconnect_session_channel(channel.get());
  ASSERT_TRUE(result->has_value());
  ASSERT_FALSE(result->value().HasValue());
  EXPECT_EQ(result->value().Error().Code(), ErrorCode::kReceiverGone);
  EXPECT_EQ(session_pending_count(), 0u);
  auto request = DecodeSentMessages<dbapp::AccountSessionRequest>(*channel).front();
  dbapp::AccountSessionReply late;
  late.request_id = request.request_id;
  late.requester_instance = request.process_instance;
  late.account_dbid = request.account_dbid;
  late.account_type_id = request.account_type_id;
  session_reply(channel.get(), late);
  EXPECT_FALSE(result->value().HasValue());
}

TEST_F(BaseAppRollbackTest, AccountSessionMissingRouteAndPreCancelledCallsDoNotSend) {
  auto result = std::make_shared<std::optional<Result<AccountSession>>>();
  query_session(result);
  ASSERT_TRUE(result->has_value());
  ASSERT_FALSE(result->value().HasValue());
  EXPECT_EQ(result->value().Error().Code(), ErrorCode::kReceiverGone);
  InterfaceTable table;
  auto channel = make_intrusive<CapturingChannel>(dispatcher_, table, Address("127.0.0.1", 24002));
  set_legacy_dbapp_channel(channel.get());
  CancellationSource cancel;
  cancel.RequestCancellation();
  result->reset();
  query_session(result, {}, cancel.Token());
  ASSERT_TRUE(result->has_value());
  ASSERT_FALSE(result->value().HasValue());
  EXPECT_EQ(result->value().Error().Code(), ErrorCode::kCancelled);
  EXPECT_EQ(channel->send_count(), 0u);
  EXPECT_EQ(session_pending_count(), 0u);
}

TEST_F(BaseAppRollbackTest, PreparedLoginTimeoutRollsBackPreparedState) {
  register_watchers();
  seed_expired_prepared_login(77, 901);

  cleanup_expired();

  EXPECT_FALSE(has_prepared(77));
  EXPECT_FALSE(has_prepared_entity(901));
  EXPECT_EQ(prepared_timeout_total(), 1u);
  EXPECT_EQ(watcher("baseapp/prepared_login_timeout_total").value_or(""), "1");
  EXPECT_EQ(watcher("baseapp/canceled_checkout_count").value_or(""), "0");
}

TEST_F(BaseAppRollbackTest, CellAppRoutesWatcherSummarizesBoundEntities) {
  register_watchers();
  const Address addr_a(0x7F000001u, 30001);
  const Address addr_b(0x7F000001u, 30002);
  ASSERT_NE(create_cell_bound_entity(42, addr_a), nullptr);
  ASSERT_NE(create_cell_bound_entity(42, addr_a, /*has_cell_backup=*/false), nullptr);
  ASSERT_NE(create_cell_bound_entity(42, addr_b, /*has_cell_backup=*/false), nullptr);

  const auto summary = watcher("baseapp/cellapp_routes").value_or("");
  const auto addr_a_entry =
      "addr=" + addr_a.ToString() + " entities=2 payload_candidates=1 ghost_backup_candidates=1";
  const auto addr_b_entry =
      "addr=" + addr_b.ToString() + " entities=1 payload_candidates=0 ghost_backup_candidates=1";
  EXPECT_NE(summary.find("routes=2"), std::string::npos);
  EXPECT_NE(summary.find(addr_a_entry), std::string::npos);
  EXPECT_NE(summary.find(addr_b_entry), std::string::npos);
}

TEST_F(BaseAppRollbackTest, CellAppDeathLostMetricsExposeWatcherCounters) {
  register_watchers();
  const Address dead_addr(0x7F000001u, 30001);
  auto* ent = create_cell_bound_entity(42, dead_addr);
  ASSERT_NE(ent, nullptr);

  baseapp::CellAppDeath death;
  death.dead_addr = dead_addr;
  on_cellapp_death(death);

  EXPECT_FALSE(ent->HasCell());
  EXPECT_EQ(death_notifications_total(), 1u);
  EXPECT_EQ(death_restore_scheduled_total(), 0u);
  EXPECT_EQ(death_restore_payload_scheduled_total(), 0u);
  EXPECT_EQ(death_restore_ghost_backup_scheduled_total(), 0u);
  EXPECT_EQ(death_lost_total(), 1u);
  EXPECT_EQ(watcher("baseapp/cellapp_death_notifications_total").value_or(""), "1");
  EXPECT_EQ(watcher("baseapp/cellapp_death_restore_scheduled_total").value_or(""), "0");
  EXPECT_EQ(watcher("baseapp/cellapp_death_restore_payload_scheduled_total").value_or(""), "0");
  EXPECT_EQ(watcher("baseapp/cellapp_death_restore_ghost_backup_scheduled_total").value_or(""),
            "0");
  EXPECT_EQ(watcher("baseapp/cellapp_death_restored_total").value_or(""), "0");
  EXPECT_EQ(watcher("baseapp/cellapp_death_lost_total").value_or(""), "1");
  EXPECT_EQ(watcher("baseapp/cellapp_death_restore_last_elapsed_ms").value_or(""), "0");
  EXPECT_EQ(watcher("baseapp/cellapp_death_restore_max_elapsed_ms").value_or(""), "0");
  EXPECT_EQ(watcher("baseapp/cellapp_death_restore_status").value_or(""),
            "state=degraded notifications=1 scheduled=0 payload_scheduled=0 "
            "ghost_backup_scheduled=0 restored=0 lost=1 timeouts=0 pending=0 "
            "unresolved=0 last_elapsed_ms=0 max_elapsed_ms=0");
}

TEST_F(BaseAppRollbackTest, CellAppDeathRestoreStatusExposesUnresolvedScheduledWork) {
  register_watchers();
  seed_death_restore_counters(/*notifications=*/1, /*scheduled=*/2, /*restored=*/1,
                              /*lost=*/0, /*timeouts=*/0);

  EXPECT_EQ(watcher("baseapp/cellapp_death_restore_status").value_or(""),
            "state=degraded notifications=1 scheduled=2 payload_scheduled=2 "
            "ghost_backup_scheduled=0 restored=1 lost=0 timeouts=0 pending=0 "
            "unresolved=1 last_elapsed_ms=0 max_elapsed_ms=0");
}

TEST_F(BaseAppRollbackTest, CellEntityCreatedCompletesPendingDeathRestore) {
  register_watchers();
  const Address dead_addr(0x7F000001u, 30001);
  auto* ent = create_cell_bound_entity(42, dead_addr);
  ASSERT_NE(ent, nullptr);
  ent->ClearCell();
  seed_pending_death_restore(ent->EntityId());

  baseapp::CellEntityCreated created;
  created.entity_id = ent->EntityId();
  created.cell_addr = Address(0x7F000001u, 30002);
  on_cell_entity_created(created);

  EXPECT_TRUE(ent->HasCell());
  EXPECT_EQ(death_restored_total(), 1u);
  EXPECT_EQ(death_lost_total(), 0u);
  EXPECT_EQ(pending_death_restore_count(), 0u);
  EXPECT_EQ(watcher("baseapp/cellapp_death_restored_total").value_or(""), "1");
}

TEST_F(BaseAppRollbackTest, CellEntityCreateFailedCompletesPendingDeathRestoreAsLost) {
  register_watchers();
  const Address dead_addr(0x7F000001u, 30001);
  auto* ent = create_cell_bound_entity(42, dead_addr);
  ASSERT_NE(ent, nullptr);
  ent->ClearCell();
  seed_pending_death_restore(ent->EntityId());

  baseapp::CellEntityCreateFailed failed;
  failed.entity_id = ent->EntityId();
  failed.request_id = ent->EntityId();
  failed.reason = baseapp::CellEntityCreateFailureReason::kGhostRequiredMissing;
  on_cell_entity_create_failed(failed);

  EXPECT_FALSE(ent->HasCell());
  EXPECT_EQ(death_restored_total(), 0u);
  EXPECT_EQ(death_lost_total(), 1u);
  EXPECT_EQ(pending_death_restore_count(), 0u);
  EXPECT_EQ(watcher("baseapp/cellapp_death_lost_total").value_or(""), "1");
}

TEST_F(BaseAppRollbackTest, CellAppDeathRestoreTimeoutCompletesPendingAsLost) {
  register_watchers();
  const Address dead_addr(0x7F000001u, 30001);
  auto* ent = create_cell_bound_entity(42, dead_addr);
  ASSERT_NE(ent, nullptr);
  ent->ClearCell();
  seed_pending_death_restore(ent->EntityId(), expired_death_restore_start());

  sweep_death_restore_timeouts();

  EXPECT_FALSE(ent->HasCell());
  EXPECT_EQ(death_restored_total(), 0u);
  EXPECT_EQ(death_lost_total(), 1u);
  EXPECT_EQ(death_restore_timeouts_total(), 1u);
  EXPECT_EQ(pending_death_restore_count(), 0u);
  EXPECT_GE(death_restore_last_elapsed_ms(), 5000u);
  EXPECT_EQ(death_restore_max_elapsed_ms(), death_restore_last_elapsed_ms());
  EXPECT_EQ(watcher("baseapp/cellapp_death_lost_total").value_or(""), "1");
  EXPECT_EQ(watcher("baseapp/cellapp_death_restore_timeouts_total").value_or(""), "1");
  EXPECT_EQ(watcher("baseapp/cellapp_death_pending_restores").value_or(""), "0");
  EXPECT_EQ(watcher("baseapp/cellapp_death_restore_last_elapsed_ms").value_or(""),
            std::to_string(death_restore_last_elapsed_ms()));
  EXPECT_EQ(watcher("baseapp/cellapp_death_restore_max_elapsed_ms").value_or(""),
            std::to_string(death_restore_max_elapsed_ms()));
}

TEST_F(BaseAppRollbackTest, DbAppShardTableRoutesDbidAndKeepsLegacyFallback) {
  register_watchers();
  InterfaceTable channel_table;
  CapturingChannel legacy(dispatcher_, channel_table, Address(0x7F000001u, 24001));
  CapturingChannel shard(dispatcher_, channel_table, Address(0x7F000001u, 24002));
  const Address shard_addr(0x7F000001u, 24002);

  set_legacy_dbapp_channel(&legacy);
  cache_dbapp_channel(shard_addr, &shard);
  apply_dbapp_shards(5, {dbappmgr::ShardEntry{1, 1000, 8, shard_addr, false}});

  EXPECT_EQ(resolve_dbapp(42), &shard);
  EXPECT_EQ(resolve_dbapp(1000), nullptr);
  EXPECT_EQ(resolve_dbapp(kInvalidDBID), &legacy);
  EXPECT_EQ(dbapp_shard_version(), 5u);
  EXPECT_EQ(dbapp_shard_count(), 1u);
  EXPECT_EQ(watcher("baseapp/dbapp_shard_table_version").value_or(""), "5");
  EXPECT_EQ(watcher("baseapp/dbapp_shard_count").value_or(""), "1");
}

TEST_F(BaseAppRollbackTest, AsyncPersistenceCompletesOnlyOnAMatchingConfirmedAck) {
  InterfaceTable table;
  CapturingChannel db(dispatcher_, table, Address(0x7F000001, 24002));
  set_legacy_dbapp_channel(&db);
  auto* entity = create_db_entity(kInvalidDBID);
  ASSERT_NE(entity, nullptr);
  int completed = 0;
  auto wait = persistence_wait(123, [&](std::span<const std::byte> payload) {
    ++completed;
    BinaryReader reader(payload);
    EXPECT_EQ(reader.Read<uint64_t>().Value(), 123u);
    EXPECT_EQ(reader.Read<int32_t>().Value(), 0);
    EXPECT_EQ(reader.Read<DatabaseID>().Value(), 42);
    EXPECT_EQ(entity->Dbid(), 42);
  });
  ASSERT_TRUE(wait.IsValid());
  EXPECT_EQ(write_to_db_async(entity->EntityId(), 123), RpcSendResult::kQueued);
  EXPECT_EQ(completed, 0);
  const auto writes = DecodeSentMessages<dbapp::WriteEntity>(db);
  ASSERT_EQ(writes.size(), 1u);
  dbapp::WriteEntityAck ack;
  ack.request_id = writes[0].request_id;
  ack.success = true;
  ack.dbid = 42;
  handle_write_ack(ack);
  EXPECT_EQ(completed, 0);
  ack.operation_id = writes[0].operation_id;
  handle_write_ack(ack);
  handle_write_ack(ack);
  EXPECT_EQ(completed, 1);
  EXPECT_EQ(session_pending_count(), 0u);
}

TEST_F(BaseAppRollbackTest, AsyncPersistenceReportsMissingRouteAndTargetImmediately) {
  auto* entity = create_db_entity(42);
  ASSERT_NE(entity, nullptr);
  std::vector<int32_t> errors;
  auto callback = [&](std::span<const std::byte> payload) {
    BinaryReader reader(payload);
    EXPECT_TRUE(reader.Read<uint64_t>().HasValue());
    errors.push_back(reader.Read<int32_t>().Value());
  };
  ASSERT_TRUE(persistence_wait(123, callback).IsValid());
  EXPECT_EQ(write_to_db_async(entity->EntityId(), 123), RpcSendResult::kQueued);
  ASSERT_TRUE(persistence_wait(124, callback).IsValid());
  EXPECT_EQ(write_to_db_async(kInvalidEntityID, 124), RpcSendResult::kQueued);
  EXPECT_EQ(errors, (std::vector<int32_t>{-5, -4}));
  EXPECT_EQ(session_pending_count(), 0u);
  EXPECT_EQ(write_to_db_async(entity->EntityId(), 999), RpcSendResult::kInvalidTarget);
}

TEST_F(BaseAppRollbackTest, AsyncPersistenceUnknownOutcomeCompletesWaitButKeepsTheWriteFenced) {
  InterfaceTable table;
  CapturingChannel db(dispatcher_, table, Address(0x7F000001, 24002));
  set_legacy_dbapp_channel(&db);
  auto* entity = create_db_entity(42);
  ASSERT_NE(entity, nullptr);
  int completed = 0;
  ASSERT_TRUE(persistence_wait(123, [&](std::span<const std::byte> payload) {
                ++completed;
                BinaryReader reader(payload);
                EXPECT_TRUE(reader.Read<uint64_t>().HasValue());
                EXPECT_EQ(reader.Read<int32_t>().Value(), -8);
              }).IsValid());
  EXPECT_EQ(write_to_db_async(entity->EntityId(), 123), RpcSendResult::kQueued);
  const auto writes = DecodeSentMessages<dbapp::WriteEntity>(db);
  ASSERT_EQ(writes.size(), 1u);
  dbapp::WriteEntityAck ack;
  ack.request_id = writes[0].request_id;
  ack.operation_id = writes[0].operation_id;
  ack.error_kind = DatabaseErrorKind::kCommitUnknown;
  handle_write_ack(ack);
  EXPECT_EQ(completed, 1);
  EXPECT_TRUE(pending_write_to_db_contains(ack.request_id));
  EXPECT_EQ(session_pending_count(), 0u);
  ack.success = true;
  ack.error_kind = DatabaseErrorKind::kNone;
  ack.dbid = 42;
  handle_write_ack(ack);
  EXPECT_EQ(completed, 1);
  EXPECT_FALSE(pending_write_to_db_contains(ack.request_id));
}

TEST_F(BaseAppRollbackTest, AsyncPersistenceDatabaseFailureIsNotReportedAsSuccess) {
  InterfaceTable table;
  CapturingChannel db(dispatcher_, table, Address(0x7F000001, 24002));
  set_legacy_dbapp_channel(&db);
  auto* entity = create_db_entity(42);
  ASSERT_NE(entity, nullptr);
  int completed = 0;
  ASSERT_TRUE(persistence_wait(123, [&](std::span<const std::byte> payload) {
                ++completed;
                BinaryReader reader(payload);
                EXPECT_TRUE(reader.Read<uint64_t>().HasValue());
                EXPECT_EQ(reader.Read<int32_t>().Value(), -3);
                EXPECT_EQ(reader.ReadString().Value(), "injected database failure");
              }).IsValid());
  EXPECT_EQ(write_to_db_async(entity->EntityId(), 123), RpcSendResult::kQueued);
  const auto writes = DecodeSentMessages<dbapp::WriteEntity>(db);
  ASSERT_EQ(writes.size(), 1u);
  dbapp::WriteEntityAck ack;
  ack.request_id = writes[0].request_id;
  ack.operation_id = writes[0].operation_id;
  ack.error_kind = DatabaseErrorKind::kOwnershipConflict;
  ack.error = "injected database failure";
  handle_write_ack(ack);
  EXPECT_EQ(completed, 1);
  EXPECT_FALSE(pending_write_to_db_contains(ack.request_id));
  EXPECT_EQ(session_pending_count(), 0u);
}

TEST_F(BaseAppRollbackTest, AsyncPersistenceCancellationDoesNotUndoAnIssuedWrite) {
  InterfaceTable table;
  CapturingChannel db(dispatcher_, table, Address(0x7F000001, 24002));
  set_legacy_dbapp_channel(&db);
  auto* entity = create_db_entity(kInvalidDBID);
  ASSERT_NE(entity, nullptr);
  int completed = 0;
  auto wait = persistence_wait(123, [&](auto) { ++completed; });
  ASSERT_TRUE(wait.IsValid());
  EXPECT_EQ(write_to_db_async(entity->EntityId(), 123), RpcSendResult::kQueued);
  cancel_persistence_wait(wait);
  EXPECT_EQ(session_pending_count(), 0u);
  const auto writes = DecodeSentMessages<dbapp::WriteEntity>(db);
  ASSERT_EQ(writes.size(), 1u);
  dbapp::WriteEntityAck ack;
  ack.request_id = writes[0].request_id;
  ack.operation_id = writes[0].operation_id;
  ack.success = true;
  ack.dbid = 42;
  handle_write_ack(ack);
  EXPECT_EQ(entity->Dbid(), 42);
  EXPECT_EQ(completed, 0);
  EXPECT_FALSE(pending_write_to_db_contains(ack.request_id));
}

TEST_F(BaseAppRollbackTest, WriteToDbUsesShardChannelForKnownDbid) {
  InterfaceTable channel_table;
  CapturingChannel legacy(dispatcher_, channel_table, Address(0x7F000001u, 24001));
  CapturingChannel shard(dispatcher_, channel_table, Address(0x7F000001u, 24002));
  const Address shard_addr(0x7F000001u, 24002);

  set_legacy_dbapp_channel(&legacy);
  cache_dbapp_channel(shard_addr, &shard);
  apply_dbapp_shards(7, {dbappmgr::ShardEntry{1, 1000, 8, shard_addr, false}});
  auto* ent = create_db_entity(42);
  ASSERT_NE(ent, nullptr);
  ent->SetCheckoutOwner(CheckoutInfo{Address(0x7F000001, 25000), 3, 9, 123, 111});

  const std::array<std::byte, 3> blob{std::byte{0x01}, std::byte{0x02}, std::byte{0x03}};
  write_to_db(ent->EntityId(), blob);

  EXPECT_EQ(shard.send_count(), 1u);
  EXPECT_EQ(legacy.send_count(), 0u);
  const auto writes = DecodeSentMessages<dbapp::WriteEntity>(shard);
  ASSERT_EQ(writes.size(), 1u);
  EXPECT_EQ(writes[0].shard_table_version, 7u);
  EXPECT_EQ(writes[0].owner.ownership_version, 123u);
  EXPECT_EQ(writes[0].owner.entity_id, 9u);
  auto snapshot = DecodeDbBlob(writes[0].blob);
  ASSERT_TRUE(snapshot.HasValue());
  EXPECT_TRUE(std::ranges::equal(snapshot->base, blob));
  EXPECT_NE(writes[0].request_id, ent->EntityId());
  EXPECT_EQ(pending_write_to_db_count(), 1u);
  EXPECT_TRUE(pending_write_to_db_contains(writes[0].request_id));

  dbapp::WriteEntityAck ack;
  ack.operation_id = writes[0].operation_id;
  ack.request_id = writes[0].request_id;
  ack.success = true;
  ack.dbid = 42;
  ack.operation_id[0] ^= std::byte{1};
  handle_write_ack(ack);
  EXPECT_TRUE(pending_write_to_db_contains(writes[0].request_id));
  ack.operation_id = writes[0].operation_id;
  handle_write_ack(ack);
  EXPECT_FALSE(pending_write_to_db_contains(writes[0].request_id));
}

TEST_F(BaseAppRollbackTest, FirstSaveKeepsDatabaseOwnerForSubsequentWrite) {
  InterfaceTable table;
  CapturingChannel dbapp(dispatcher_, table, Address(0x7F000001, 24002));
  set_legacy_dbapp_channel(&dbapp);
  auto* entity = create_db_entity(kInvalidDBID);
  ASSERT_NE(entity, nullptr);
  const std::array<std::byte, 1> blob{std::byte{1}};
  write_to_db(entity->EntityId(), blob);
  auto writes = DecodeSentMessages<dbapp::WriteEntity>(dbapp);
  ASSERT_EQ(writes.size(), 1u);
  ASSERT_TRUE(HasFlag(writes[0].flags, WriteFlags::kCreateNew));
  write_to_db(entity->EntityId(), blob);
  EXPECT_EQ(DecodeSentMessages<dbapp::WriteEntity>(dbapp).size(), 1u);

  dbapp::WriteEntityAck ack;
  ack.operation_id = writes[0].operation_id;
  ack.request_id = writes[0].request_id;
  ack.dbid = 42;
  ack.success = true;
  ack.owner = CheckoutInfo{Address(0x7F000001, 25001), 7, entity->EntityId(), 123, 111};
  handle_write_ack(ack);
  ASSERT_TRUE(entity->CheckoutOwner().has_value());
  EXPECT_EQ(entity->CheckoutOwner()->ownership_version, 123u);
  write_to_db(entity->EntityId(), blob);
  writes = DecodeSentMessages<dbapp::WriteEntity>(dbapp);
  ASSERT_EQ(writes.size(), 2u);
  EXPECT_EQ(writes[1].dbid, 42);
  EXPECT_EQ(writes[1].owner.ownership_version, 123u);
}

TEST_F(BaseAppRollbackTest, ShutdownConfirmsPersistenceBeforeDestroyingAndReleasingOwner) {
  InterfaceTable table;
  CapturingChannel database(dispatcher_, table, Address("127.0.0.1", 24002));
  set_legacy_dbapp_channel(&database);
  auto* entity = create_db_entity(42);
  ASSERT_NE(entity, nullptr);
  const auto id = entity->EntityId();
  const CheckoutInfo owner{Address("127.0.0.1", 25001), 7, id, 9, 111};
  entity->SetCheckoutOwner(owner);
  entity->SetEntityData(std::vector<std::byte>{std::byte{17}});
  begin_shutdown();
  const auto writes = DecodeSentMessages<dbapp::WriteEntity>(database);
  ASSERT_EQ(writes.size(), 1u);
  EXPECT_TRUE(HasFlag(writes[0].flags, WriteFlags::kLogOff));
  EXPECT_EQ(writes[0].owner, owner);
  EXPECT_EQ(shutdown_saves(), 1u);
  EXPECT_FALSE(entity->IsPendingDestroy());
  dbapp::WriteEntityAck ack;
  ack.operation_id = writes[0].operation_id;
  ack.request_id = writes[0].request_id;
  ack.dbid = 42;
  ack.success = true;
  handle_write_ack(ack);
  EXPECT_EQ(shutdown_saves(), 0u);
  EXPECT_FALSE(shutdown_failed());
  EXPECT_EQ(find_db_entity(42), nullptr);
  EXPECT_TRUE(entity->IsPendingDestroy());
}

TEST_F(BaseAppRollbackTest, ShutdownRetainsUnconfirmedSaveAndOwnerAfterUnknownCommit) {
  InterfaceTable table;
  CapturingChannel database(dispatcher_, table, Address("127.0.0.1", 24002));
  set_legacy_dbapp_channel(&database);
  auto* entity = create_db_entity(42);
  ASSERT_NE(entity, nullptr);
  const CheckoutInfo owner{Address("127.0.0.1", 25001), 7, entity->EntityId(), 9, 111};
  entity->SetCheckoutOwner(owner);
  begin_shutdown();
  const auto writes = DecodeSentMessages<dbapp::WriteEntity>(database);
  ASSERT_EQ(writes.size(), 1u);
  dbapp::WriteEntityAck ack;
  ack.operation_id = writes[0].operation_id;
  ack.request_id = writes[0].request_id;
  ack.error_kind = DatabaseErrorKind::kCommitUnknown;
  ack.error = "test commit outcome unknown";
  handle_write_ack(ack);
  EXPECT_EQ(shutdown_saves(), 1u);
  EXPECT_EQ(entity->CheckoutOwner(), owner);
  EXPECT_FALSE(entity->IsPendingDestroy());
}

TEST_F(BaseAppRollbackTest, ShutdownReportsKnownSaveFailureWithoutRetryingOrReleasingOwner) {
  InterfaceTable table;
  CapturingChannel database(dispatcher_, table, Address("127.0.0.1", 24002));
  set_legacy_dbapp_channel(&database);
  auto* entity = create_db_entity(42);
  ASSERT_NE(entity, nullptr);
  const CheckoutInfo owner{Address("127.0.0.1", 25001), 7, entity->EntityId(), 9, 111};
  entity->SetCheckoutOwner(owner);
  begin_shutdown();
  const auto writes = DecodeSentMessages<dbapp::WriteEntity>(database);
  ASSERT_EQ(writes.size(), 1u);
  dbapp::WriteEntityAck ack;
  ack.operation_id = writes[0].operation_id;
  ack.request_id = writes[0].request_id;
  ack.error = "test write rejected";
  handle_write_ack(ack);
  EXPECT_TRUE(shutdown_failed());
  EXPECT_EQ(shutdown_saves(), 0u);
  EXPECT_EQ(entity->CheckoutOwner(), owner);
  EXPECT_FALSE(entity->IsPendingDestroy());
  shutdown_tick();
  const std::array<std::byte, 1> blob{std::byte{1}};
  write_to_db(entity->EntityId(), blob);
  EXPECT_EQ(DecodeSentMessages<dbapp::WriteEntity>(database).size(), 1u);
}

TEST_F(BaseAppRollbackTest, TimedOutFirstSaveRetainsOperationAndAcceptsLateConfirmation) {
  InterfaceTable table;
  CapturingChannel dbapp(dispatcher_, table, Address(0x7F000001, 24002));
  set_legacy_dbapp_channel(&dbapp);
  auto* entity = create_db_entity(kInvalidDBID);
  ASSERT_NE(entity, nullptr);
  const std::array<std::byte, 1> blob{std::byte{1}};
  write_to_db(entity->EntityId(), blob);
  const auto writes = DecodeSentMessages<dbapp::WriteEntity>(dbapp);
  ASSERT_EQ(writes.size(), 1u);
  expire_write(writes[0].request_id);
  cleanup_expired();
  ASSERT_TRUE(pending_write_to_db_contains(writes[0].request_id));
  start_disconnect_logoff(entity->EntityId());
  EXPECT_FALSE(entity->IsPendingDestroy());
  write_to_db(entity->EntityId(), blob);
  for (const auto& retry : DecodeSentMessages<dbapp::WriteEntity>(dbapp)) {
    EXPECT_EQ(retry.operation_id, writes[0].operation_id);
  }
  dbapp::WriteEntityAck ack;
  ack.operation_id = writes[0].operation_id;
  ack.request_id = writes[0].request_id;
  ack.success = true;
  ack.dbid = 42;
  handle_write_ack(ack);
  EXPECT_FALSE(pending_write_to_db_contains(writes[0].request_id));
  EXPECT_EQ(entity->Dbid(), 42);
  const auto completed = DecodeSentMessages<dbapp::WriteEntity>(dbapp);
  ASSERT_FALSE(completed.empty());
  EXPECT_TRUE(HasFlag(completed.back().flags, WriteFlags::kLogOff));
  EXPECT_NE(completed.back().operation_id, writes[0].operation_id);
  EXPECT_TRUE(logoff_in_flight_contains(entity->EntityId()));
}

TEST_F(BaseAppRollbackTest, UncertainFirstSaveStaysFencedAfterBoundedRetries) {
  InterfaceTable table;
  CapturingChannel dbapp(dispatcher_, table, Address(0x7F000001, 24002));
  set_legacy_dbapp_channel(&dbapp);
  auto* entity = create_db_entity(kInvalidDBID);
  ASSERT_NE(entity, nullptr);
  const std::array<std::byte, 1> blob{std::byte{1}};
  write_to_db(entity->EntityId(), blob);
  const auto writes = DecodeSentMessages<dbapp::WriteEntity>(dbapp);
  ASSERT_EQ(writes.size(), 1u);
  dbapp::WriteEntityAck ack;
  ack.operation_id = writes[0].operation_id;
  ack.request_id = writes[0].request_id;
  ack.error_kind = DatabaseErrorKind::kCommitUnknown;
  ack.error = "commit outcome unknown";
  for (int i = 0; i < 6; ++i) {
    handle_write_ack(ack);
    if (i == 0) {
      EXPECT_EQ(DecodeSentMessages<dbapp::WriteEntity>(dbapp).size(), 1u);
    }
    retry_writes();
  }
  ASSERT_TRUE(pending_write_to_db_contains(ack.request_id));
  auto retries = DecodeSentMessages<dbapp::WriteEntity>(dbapp);
  ASSERT_EQ(retries.size(), 4u);
  for (const auto& retry : retries) {
    EXPECT_EQ(retry.operation_id, writes[0].operation_id);
    EXPECT_EQ(retry.blob, writes[0].blob);
  }
  ack.error_kind = DatabaseErrorKind::kNone;
  ack.error = "database unavailable";
  handle_write_ack(ack);
  expire_write(ack.request_id);
  cleanup_expired();
  write_to_db(entity->EntityId(), blob);
  EXPECT_TRUE(pending_write_to_db_contains(ack.request_id));
  EXPECT_EQ(DecodeSentMessages<dbapp::WriteEntity>(dbapp).size(), 4u);
  ack.success = true;
  ack.dbid = 42;
  ack.error.clear();
  handle_write_ack(ack);
  EXPECT_EQ(entity->Dbid(), 42);
  EXPECT_FALSE(pending_write_to_db_contains(ack.request_id));
  write_to_db(entity->EntityId(), blob);
  retries = DecodeSentMessages<dbapp::WriteEntity>(dbapp);
  ASSERT_EQ(retries.size(), 5u);
  EXPECT_EQ(retries.back().dbid, 42);
  EXPECT_NE(retries.back().operation_id, writes[0].operation_id);
}

TEST_F(BaseAppRollbackTest, UncertainLogoffKeepsEntityAndLeaseUntilConfirmed) {
  InterfaceTable table;
  CapturingChannel dbapp(dispatcher_, table, Address(0x7F000001, 24002));
  set_legacy_dbapp_channel(&dbapp);
  auto* entity = create_db_entity(42);
  ASSERT_NE(entity, nullptr);
  entity->SetEntityData({std::byte{1}});
  start_disconnect_logoff(entity->EntityId());
  auto writes = DecodeSentMessages<dbapp::WriteEntity>(dbapp);
  ASSERT_EQ(writes.size(), 1u);
  dbapp::WriteEntityAck ack;
  ack.operation_id = writes[0].operation_id;
  ack.request_id = writes[0].request_id;
  ack.dbid = 42;
  ack.error_kind = DatabaseErrorKind::kCommitUnknown;
  for (int i = 0; i < 6; ++i) {
    handle_write_ack(ack);
    retry_writes();
  }
  EXPECT_TRUE(pending_logoff_write_contains(ack.request_id));
  EXPECT_TRUE(logoff_in_flight_contains(entity->EntityId()));
  EXPECT_FALSE(entity->IsPendingDestroy());
  handle_dbapp_death(dbapp.RemoteAddress());
  EXPECT_TRUE(pending_logoff_write_contains(ack.request_id));
  EXPECT_TRUE(logoff_in_flight_contains(entity->EntityId()));
  start_disconnect_logoff(entity->EntityId());
  write_to_db(entity->EntityId(), entity->EntityData());
  writes = DecodeSentMessages<dbapp::WriteEntity>(dbapp);
  ASSERT_EQ(writes.size(), 4u);
  for (const auto& retry : writes) EXPECT_EQ(retry.operation_id, ack.operation_id);
  EXPECT_TRUE(DecodeSentMessages<dbapp::CheckinEntity>(dbapp).empty());
  ack.success = true;
  ack.error_kind = DatabaseErrorKind::kNone;
  handle_write_ack(ack);
  EXPECT_FALSE(pending_logoff_write_contains(ack.request_id));
  EXPECT_FALSE(logoff_in_flight_contains(entity->EntityId()));
  EXPECT_TRUE(entity->IsPendingDestroy());
}

TEST_F(BaseAppRollbackTest, OrdinarySaveFailureClearsPendingOperation) {
  InterfaceTable table;
  CapturingChannel dbapp(dispatcher_, table, Address(0x7F000001, 24002));
  set_legacy_dbapp_channel(&dbapp);
  auto* entity = create_db_entity(42);
  ASSERT_NE(entity, nullptr);
  const std::array<std::byte, 1> blob{std::byte{1}};
  write_to_db(entity->EntityId(), blob);
  const auto writes = DecodeSentMessages<dbapp::WriteEntity>(dbapp);
  ASSERT_EQ(writes.size(), 1u);
  dbapp::WriteEntityAck ack;
  ack.operation_id = writes[0].operation_id;
  ack.request_id = writes[0].request_id;
  ack.error = "execution failed before commit";
  handle_write_ack(ack);
  EXPECT_FALSE(pending_write_to_db_contains(ack.request_id));
  write_to_db(entity->EntityId(), blob);
  EXPECT_EQ(DecodeSentMessages<dbapp::WriteEntity>(dbapp).size(), 2u);
}

TEST_F(BaseAppRollbackTest, WriteToDbRetriesPendingWriteAfterDbAppDeathAndShardUpdate) {
  InterfaceTable channel_table;
  CapturingChannel old_shard(dispatcher_, channel_table, Address(0x7F000001u, 24002));
  CapturingChannel new_shard(dispatcher_, channel_table, Address(0x7F000001u, 24003));
  const Address old_addr(0x7F000001u, 24002);
  const Address new_addr(0x7F000001u, 24003);

  cache_dbapp_channel(old_addr, &old_shard);
  apply_dbapp_shards(7, {dbappmgr::ShardEntry{1, 1000, 8, old_addr, false}});
  auto* ent = create_db_entity(42);
  ASSERT_NE(ent, nullptr);

  const std::array<std::byte, 3> blob{std::byte{0x04}, std::byte{0x05}, std::byte{0x06}};
  write_to_db(ent->EntityId(), blob);

  const auto first_writes = DecodeSentMessages<dbapp::WriteEntity>(old_shard);
  ASSERT_EQ(first_writes.size(), 1u);
  const uint32_t request_id = first_writes[0].request_id;
  EXPECT_TRUE(pending_write_to_db_contains(request_id));

  handle_dbapp_death(old_addr);
  EXPECT_TRUE(pending_write_to_db_contains(request_id));

  cache_dbapp_channel(new_addr, &new_shard);
  apply_dbapp_shards(8, {dbappmgr::ShardEntry{1, 1000, 9, new_addr, false}});

  const auto retried_writes = DecodeSentMessages<dbapp::WriteEntity>(new_shard);
  ASSERT_EQ(retried_writes.size(), 1u);
  EXPECT_EQ(retried_writes[0].request_id, request_id);
  EXPECT_EQ(retried_writes[0].operation_id, first_writes[0].operation_id);
  EXPECT_EQ(retried_writes[0].shard_table_version, 8u);
  EXPECT_EQ(retried_writes[0].dbid, 42);
  EXPECT_EQ(retried_writes[0].blob, first_writes[0].blob);
  auto decoded = DecodeDbBlob(retried_writes[0].blob);
  ASSERT_TRUE(decoded.HasValue());
  EXPECT_TRUE(std::ranges::equal(decoded->base, blob));

  dbapp::WriteEntityAck ack;
  ack.operation_id = first_writes[0].operation_id;
  ack.request_id = request_id;
  ack.success = true;
  ack.dbid = 42;
  handle_write_ack(ack);
  EXPECT_FALSE(pending_write_to_db_contains(request_id));
}

TEST_F(BaseAppRollbackTest, DeferredRollbackReleasesItsLeaseInsteadOfCurrentEntityLease) {
  InterfaceTable table;
  CapturingChannel dbapp(dispatcher_, table, Address(0x7F000001, 24002));
  set_legacy_dbapp_channel(&dbapp);
  auto* entity = create_db_entity(42);
  ASSERT_NE(entity, nullptr);
  CheckoutInfo previous{Address(0x7F000001, 25001), 3, 9, 11, 111};
  auto current = previous;
  current.ownership_version = 12;
  entity->SetCheckoutOwner(current);
  fail_deferred_login(entity->EntityId(), 42, previous);
  auto releases = DecodeSentMessages<dbapp::CheckinEntity>(dbapp);
  ASSERT_EQ(releases.size(), 1u);
  EXPECT_EQ(releases[0].owner.ownership_version, 11u);
  EXPECT_EQ(entity->CheckoutOwner()->ownership_version, 12u);
}

TEST_F(BaseAppRollbackTest, LogoffPersistFailureDoesNotReleaseCheckoutOrDestroyEntity) {
  InterfaceTable table;
  CapturingChannel dbapp(dispatcher_, table, Address(0x7F000001u, 24002));
  set_legacy_dbapp_channel(&dbapp);
  auto* entity = create_db_entity(42);
  ASSERT_NE(entity, nullptr);
  entity->SetEntityData({std::byte{0x11}});
  const auto entity_id = entity->EntityId();
  start_disconnect_logoff(entity_id);
  const auto writes = DecodeSentMessages<dbapp::WriteEntity>(dbapp);
  ASSERT_EQ(writes.size(), 1u);
  EXPECT_TRUE(HasFlag(writes[0].flags, WriteFlags::kLogOff));
  EXPECT_TRUE(DecodeSentMessages<dbapp::CheckinEntity>(dbapp).empty());
  dbapp::WriteEntityAck ack;
  ack.operation_id = writes[0].operation_id;
  ack.request_id = writes[0].request_id;
  ack.dbid = 42;
  ack.error = "injected commit failure";
  handle_write_ack(ack);
  EXPECT_FALSE(entity->IsPendingDestroy());
  EXPECT_EQ(entity->Dbid(), 42);
  EXPECT_FALSE(logoff_in_flight_contains(entity_id));
  EXPECT_TRUE(DecodeSentMessages<dbapp::CheckinEntity>(dbapp).empty());
}

TEST_F(BaseAppRollbackTest, LogoffPersistRetriesPendingWriteAfterDbAppDeathAndShardUpdate) {
  InterfaceTable channel_table;
  CapturingChannel old_shard(dispatcher_, channel_table, Address(0x7F000001u, 24002));
  CapturingChannel new_shard(dispatcher_, channel_table, Address(0x7F000001u, 24003));
  const Address old_addr(0x7F000001u, 24002);
  const Address new_addr(0x7F000001u, 24003);

  cache_dbapp_channel(old_addr, &old_shard);
  apply_dbapp_shards(7, {dbappmgr::ShardEntry{1, 1000, 8, old_addr, false}});
  auto* ent = create_db_entity(42);
  ASSERT_NE(ent, nullptr);
  ent->SetEntityData({std::byte{0x11}, std::byte{0x12}});
  const EntityID entity_id = ent->EntityId();

  start_disconnect_logoff(entity_id);

  const auto first_writes = DecodeSentMessages<dbapp::WriteEntity>(old_shard);
  ASSERT_EQ(first_writes.size(), 1u);
  const uint32_t request_id = first_writes[0].request_id;
  EXPECT_EQ(first_writes[0].entity_id, entity_id);
  EXPECT_EQ(first_writes[0].dbid, 42);
  EXPECT_EQ(first_writes[0].shard_table_version, 7u);
  EXPECT_TRUE(HasFlag(first_writes[0].flags, WriteFlags::kLogOff));
  EXPECT_TRUE(DecodeSentMessages<dbapp::CheckinEntity>(old_shard).empty());
  EXPECT_TRUE(pending_logoff_write_contains(request_id));
  EXPECT_TRUE(logoff_in_flight_contains(entity_id));
  EXPECT_FALSE(ent->IsPendingDestroy());

  handle_dbapp_death(old_addr);
  EXPECT_TRUE(pending_logoff_write_contains(request_id));
  EXPECT_TRUE(pending_logoff_write_retry_pending(request_id));
  EXPECT_TRUE(logoff_in_flight_contains(entity_id));
  EXPECT_FALSE(ent->IsPendingDestroy());

  cache_dbapp_channel(new_addr, &new_shard);
  apply_dbapp_shards(8, {dbappmgr::ShardEntry{1, 1000, 9, new_addr, false}});

  const auto retried_writes = DecodeSentMessages<dbapp::WriteEntity>(new_shard);
  ASSERT_EQ(retried_writes.size(), 1u);
  EXPECT_EQ(retried_writes[0].request_id, request_id);
  EXPECT_EQ(retried_writes[0].operation_id, first_writes[0].operation_id);
  EXPECT_EQ(retried_writes[0].entity_id, entity_id);
  EXPECT_EQ(retried_writes[0].dbid, 42);
  EXPECT_EQ(retried_writes[0].shard_table_version, 8u);
  EXPECT_EQ(retried_writes[0].blob, first_writes[0].blob);
  EXPECT_FALSE(pending_logoff_write_retry_pending(request_id));
  EXPECT_TRUE(HasFlag(retried_writes[0].flags, WriteFlags::kLogOff));
  EXPECT_TRUE(DecodeSentMessages<dbapp::CheckinEntity>(new_shard).empty());

  dbapp::WriteEntityAck ack;
  ack.operation_id = first_writes[0].operation_id;
  ack.request_id = request_id;
  ack.success = true;
  ack.dbid = 42;
  handle_write_ack(ack);

  EXPECT_FALSE(pending_logoff_write_contains(request_id));
  EXPECT_FALSE(logoff_in_flight_contains(entity_id));
  EXPECT_TRUE(ent->IsPendingDestroy());
  EXPECT_EQ(ent->Dbid(), kInvalidDBID);
}

TEST_F(BaseAppRollbackTest, PrepareLoginCheckoutRetriesAfterDbAppDeathAndShardUpdate) {
  InterfaceTable channel_table;
  CapturingChannel old_shard(dispatcher_, channel_table, Address(0x7F000001u, 24002));
  CapturingChannel new_shard(dispatcher_, channel_table, Address(0x7F000001u, 24003));
  const Address old_addr(0x7F000001u, 24002);
  const Address new_addr(0x7F000001u, 24003);

  cache_dbapp_channel(old_addr, &old_shard);
  apply_dbapp_shards(7, {dbappmgr::ShardEntry{1, 1000, 8, old_addr, false}});

  const uint32_t request_id = submit_prepare_login(42);

  const auto first_checkouts = DecodeSentMessages<dbapp::CheckoutEntity>(old_shard);
  ASSERT_EQ(first_checkouts.size(), 1u);
  EXPECT_EQ(first_checkouts[0].request_id, request_id);
  EXPECT_EQ(first_checkouts[0].dbid, 42);
  EXPECT_EQ(first_checkouts[0].type_id, 7);
  EXPECT_EQ(first_checkouts[0].shard_table_version, 7u);
  EXPECT_TRUE(pending_login_contains(request_id));
  EXPECT_TRUE(active_login_contains(42));

  handle_dbapp_death(old_addr);
  EXPECT_TRUE(pending_login_contains(request_id));
  EXPECT_TRUE(pending_login_retry_pending(request_id));
  EXPECT_TRUE(active_login_contains(42));

  cache_dbapp_channel(new_addr, &new_shard);
  apply_dbapp_shards(8, {dbappmgr::ShardEntry{1, 1000, 9, new_addr, false}});

  const auto retried_checkouts = DecodeSentMessages<dbapp::CheckoutEntity>(new_shard);
  ASSERT_EQ(retried_checkouts.size(), 1u);
  EXPECT_EQ(retried_checkouts[0].request_id, request_id);
  EXPECT_EQ(retried_checkouts[0].dbid, 42);
  EXPECT_EQ(retried_checkouts[0].type_id, 7);
  EXPECT_EQ(retried_checkouts[0].shard_table_version, 8u);
  EXPECT_EQ(retried_checkouts[0].operation_id, first_checkouts[0].operation_id);
  EXPECT_NE(first_checkouts[0].operation_id, (std::array<std::byte, 16>{}));
  EXPECT_TRUE(pending_login_contains(request_id));
  EXPECT_FALSE(pending_login_retry_pending(request_id));
  EXPECT_TRUE(active_login_contains(42));
}

TEST_F(BaseAppRollbackTest, FailedCheckoutCancellationKeepsLateReplyGuard) {
  seed_canceled_checkout(42);
  handle_abort_ack(42, false);
  EXPECT_TRUE(canceled_checkout_contains(42));
  handle_abort_ack(42, true);
  EXPECT_FALSE(canceled_checkout_contains(42));
}

TEST_F(BaseAppRollbackTest, DatabaseLoadsKeepDistinctTrackedRequestsAndOperations) {
  install_checkout_type();
  InterfaceTable table;
  CapturingChannel channel(dispatcher_, table, Address("127.0.0.1", 24001));
  set_legacy_dbapp_channel(&channel);
  ASSERT_NE(create_db_entity(1), nullptr);
  baseapp::CreateBaseFromDB request;
  request.type_id = 7;
  request.dbid = 42;
  load_from_db(channel, request);
  request.dbid = kInvalidDBID;
  request.identifier = "named";
  load_from_db(channel, request);
  const auto checkouts = DecodeSentMessages<dbapp::CheckoutEntity>(channel);
  ASSERT_EQ(checkouts.size(), 2u);
  EXPECT_NE(checkouts[0].entity_id, checkouts[1].entity_id);
  EXPECT_NE(checkouts[0].request_id, 0u);
  EXPECT_NE(checkouts[0].request_id, checkouts[1].request_id);
  EXPECT_NE(checkouts[0].operation_id, (std::array<std::byte, 16>{}));
  EXPECT_NE(checkouts[0].operation_id, checkouts[1].operation_id);
  EXPECT_EQ(checkouts[1].mode, dbapp::LoadMode::kByName);
}

TEST_F(BaseAppRollbackTest, NamedDatabaseLoadIndexesConfirmedIdentityAndIgnoresDuplicates) {
  InterfaceTable table;
  const Address source("127.0.0.1", 24001);
  CapturingChannel channel(dispatcher_, table, source);
  set_legacy_dbapp_channel(&channel);
  set_app_id(7);
  ASSERT_NE(create_db_entity(1), nullptr);
  baseapp::CreateBaseFromDB request;
  request.type_id = 7;
  request.identifier = "named";
  load_from_db(channel, request);
  const auto requests = DecodeSentMessages<dbapp::CheckoutEntity>(channel);
  ASSERT_EQ(requests.size(), 1u);
  const auto& checkout = requests[0];
  dbapp::CheckoutEntityAck reply;
  reply.blob = DatabaseBlob();
  reply.request_id = checkout.request_id;
  reply.operation_id = checkout.operation_id;
  reply.dbid = 42;
  reply.checkout_version = 11;
  reply.holder_app_id = checkout.owner_app_id;
  reply.holder_process_instance = 111;
  reply.holder_entity_id = checkout.entity_id;
  reply.holder_process_instance = 111;
  reply.blob = DatabaseBlob({std::byte{0x11}}, {std::byte{0x22}});
  handle_checkout_ack(source, reply);
  ASSERT_FALSE(native_checkout_contains(checkout.request_id));
  auto* entity = find_db_entity(42);
  ASSERT_NE(entity, nullptr);
  EXPECT_EQ(entity->EntityId(), checkout.entity_id);
  EXPECT_EQ(entity->EntityData(), (std::vector<std::byte>{std::byte{0x11}}));
  EXPECT_EQ(entity->CellBackupData(), (std::vector<std::byte>{std::byte{0x22}}));
  ASSERT_TRUE(entity->CheckoutOwner());
  EXPECT_EQ(entity->CheckoutOwner()->ownership_version, 11u);
  EXPECT_FALSE(active_login_contains(42));
  EXPECT_FALSE(canceled_checkout_contains(checkout.request_id));
  reply.status = dbapp::CheckoutStatus::kDbError;
  handle_checkout_ack(source, reply);
  EXPECT_EQ(find_db_entity(42), entity);
  EXPECT_TRUE(DecodeSentMessages<dbapp::AbortCheckout>(channel).empty());
  write_to_db(checkout.entity_id, entity->EntityData());
  const auto writes = DecodeSentMessages<dbapp::WriteEntity>(channel);
  ASSERT_EQ(writes.size(), 1u);
  EXPECT_EQ(writes[0].dbid, 42);
  EXPECT_EQ(writes[0].owner.ownership_version, 11u);
}

TEST_F(BaseAppRollbackTest, UnversionedDatabaseLoadIsRejectedAndCheckoutIsAborted) {
  InterfaceTable table;
  const Address source("127.0.0.1", 24001);
  CapturingChannel channel(dispatcher_, table, source);
  set_legacy_dbapp_channel(&channel);
  set_app_id(7);
  ASSERT_NE(create_db_entity(1), nullptr);
  baseapp::CreateBaseFromDB request;
  request.type_id = 7;
  request.identifier = "named";
  load_from_db(channel, request);
  const auto requests = DecodeSentMessages<dbapp::CheckoutEntity>(channel);
  ASSERT_EQ(requests.size(), 1u);
  const auto& checkout = requests[0];
  dbapp::CheckoutEntityAck reply;
  reply.blob = DatabaseBlob();
  reply.request_id = checkout.request_id;
  reply.operation_id = checkout.operation_id;
  reply.dbid = 42;
  reply.checkout_version = 11;
  reply.holder_app_id = checkout.owner_app_id;
  reply.holder_process_instance = 111;
  reply.holder_entity_id = checkout.entity_id;
  reply.blob = {std::byte{0x11}};
  handle_checkout_ack(source, reply);
  EXPECT_EQ(find_db_entity(42), nullptr);
  const auto aborted = DecodeSentMessages<dbapp::AbortCheckout>(channel);
  ASSERT_EQ(aborted.size(), 1u);
  EXPECT_EQ(aborted[0].operation_id, checkout.operation_id);
}

TEST_F(BaseAppRollbackTest, UncertainCheckoutRetriesSameOperationAndKeepsAccountFenced) {
  InterfaceTable table;
  const Address source("127.0.0.1", 24001);
  CapturingChannel channel(dispatcher_, table, source);
  set_legacy_dbapp_channel(&channel);
  set_app_id(7);
  const auto request_id = submit_prepare_login(42);
  const auto initial = DecodeSentMessages<dbapp::CheckoutEntity>(channel);
  ASSERT_EQ(initial.size(), 1u);
  dbapp::CheckoutEntityAck reply;
  reply.blob = DatabaseBlob();
  reply.operation_id = initial[0].operation_id;
  reply.request_id = request_id;
  reply.status = dbapp::CheckoutStatus::kDbError;
  reply.error_kind = DatabaseErrorKind::kCommitUnknown;
  handle_checkout_ack(Address("127.0.0.1", 24002), reply);
  EXPECT_FALSE(pending_login_retry_pending(request_id));
  auto forged = reply;
  forged.operation_id[0] ^= std::byte{1};
  handle_checkout_ack(source, forged);
  EXPECT_FALSE(pending_login_retry_pending(request_id));
  handle_checkout_ack(source, reply);
  ASSERT_TRUE(pending_login_contains(request_id));
  EXPECT_TRUE(pending_login_retry_pending(request_id));
  EXPECT_TRUE(active_login_contains(42));
  set_app_id(9);
  retry_checkout_now(request_id);
  const auto retried = DecodeSentMessages<dbapp::CheckoutEntity>(channel);
  ASSERT_EQ(retried.size(), 2u);
  EXPECT_EQ(retried[1].operation_id, initial[0].operation_id);
  EXPECT_EQ(retried[1].owner_app_id, 7u);
  reply.status = dbapp::CheckoutStatus::kNotFound;
  reply.error_kind = DatabaseErrorKind::kNotFound;
  handle_checkout_ack(source, reply);
  EXPECT_FALSE(pending_login_contains(request_id));
  ASSERT_TRUE(canceled_checkout_contains(request_id));
  EXPECT_TRUE(active_login_contains(42));
  auto aborts = DecodeSentMessages<dbapp::AbortCheckout>(channel);
  ASSERT_EQ(aborts.size(), 1u);
  EXPECT_EQ(aborts[0].operation_id, initial[0].operation_id);
  EXPECT_EQ(aborts[0].owner_app_id, 7u);
  for (int i = 0; i < 4; ++i) retry_cancel_now(request_id);
  EXPECT_TRUE(checkout_cancel_stalled(request_id));
  cleanup_expired();
  EXPECT_TRUE(canceled_checkout_contains(request_id));
  EXPECT_TRUE(active_login_contains(42));
  reply.status = dbapp::CheckoutStatus::kSuccess;
  reply.error_kind = DatabaseErrorKind::kNone;
  reply.dbid = 42;
  reply.checkout_version = 11;
  reply.holder_app_id = 7;
  reply.holder_process_instance = 111;
  handle_checkout_ack(Address("127.0.0.1", 24002), reply);
  EXPECT_TRUE(checkout_cancel_stalled(request_id));
  handle_checkout_ack(source, reply);
  EXPECT_FALSE(checkout_cancel_stalled(request_id));
  dbapp::AbortCheckoutAck canceled;
  canceled.request_id = request_id;
  canceled.operation_id = initial[0].operation_id;
  canceled.success = true;
  handle_abort_result(Address("127.0.0.1", 24002), canceled);
  EXPECT_TRUE(canceled_checkout_contains(request_id));
  handle_abort_result(source, canceled);
  EXPECT_FALSE(canceled_checkout_contains(request_id));
  EXPECT_FALSE(active_login_contains(42));
}

TEST_F(BaseAppRollbackTest, CheckoutRequestIdWrapDoesNotReplaceWaitingCalls) {
  InterfaceTable table;
  CapturingChannel channel(dispatcher_, table, Address("127.0.0.1", 24001));
  set_legacy_dbapp_channel(&channel);
  set_request_counter(std::numeric_limits<uint32_t>::max());
  (void)submit_prepare_login(42);
  (void)submit_prepare_login(43);
  set_request_counter(std::numeric_limits<uint32_t>::max());
  (void)submit_prepare_login(44);
  const auto requests = DecodeSentMessages<dbapp::CheckoutEntity>(channel);
  ASSERT_EQ(requests.size(), 3u);
  EXPECT_EQ(requests[0].request_id, std::numeric_limits<uint32_t>::max());
  EXPECT_EQ(requests[1].request_id, 1u);
  EXPECT_EQ(requests[2].request_id, 2u);
  for (const auto& request : requests) EXPECT_TRUE(pending_login_contains(request.request_id));
}

TEST_F(BaseAppRollbackTest, ConfirmedCheckoutRollbackWaitsForReleaseConfirmation) {
  InterfaceTable table;
  const Address source("127.0.0.1", 24001);
  CapturingChannel channel(dispatcher_, table, source);
  set_legacy_dbapp_channel(&channel);
  const auto request_id = submit_prepare_login(42);
  const auto requests = DecodeSentMessages<dbapp::CheckoutEntity>(channel);
  ASSERT_EQ(requests.size(), 1u);
  dbapp::CheckoutEntityAck reply;
  reply.blob = DatabaseBlob();
  reply.operation_id = requests[0].operation_id;
  reply.request_id = request_id;
  reply.status = dbapp::CheckoutStatus::kSuccess;
  reply.dbid = 42;
  reply.checkout_version = 7;
  reply.holder_app_id = requests[0].owner_app_id;
  reply.holder_process_instance = 111;
  handle_checkout_ack(source, reply);
  EXPECT_FALSE(pending_login_contains(request_id));
  EXPECT_TRUE(canceled_checkout_contains(request_id));
  EXPECT_TRUE(active_login_contains(42));
  EXPECT_TRUE(DecodeSentMessages<dbapp::CheckinEntity>(channel).empty());
  const auto aborts = DecodeSentMessages<dbapp::AbortCheckout>(channel);
  ASSERT_EQ(aborts.size(), 1u);
  EXPECT_EQ(aborts[0].operation_id, requests[0].operation_id);
  dbapp::AbortCheckoutAck released;
  released.operation_id = reply.operation_id;
  released.request_id = request_id;
  released.success = true;
  handle_abort_result(source, released);
  EXPECT_FALSE(canceled_checkout_contains(request_id));
  EXPECT_FALSE(active_login_contains(42));
}

TEST_F(BaseAppRollbackTest, NativeCheckoutUncertaintyDoesNotDestroyOrReleaseOtherEntities) {
  install_checkout_type();
  InterfaceTable table;
  const Address source("127.0.0.1", 24001);
  CapturingChannel channel(dispatcher_, table, source);
  set_legacy_dbapp_channel(&channel);
  auto* unrelated = create_db_entity(1);
  ASSERT_NE(unrelated, nullptr);
  const auto unrelated_id = unrelated->EntityId();
  baseapp::CreateBaseFromDB request;
  request.type_id = 7;
  request.dbid = 42;
  load_from_db(channel, request);
  const auto outgoing = DecodeSentMessages<dbapp::CheckoutEntity>(channel);
  ASSERT_EQ(outgoing.size(), 1u);
  const std::array<std::byte, 1> premature_data{std::byte{1}};
  write_to_db(outgoing[0].entity_id, premature_data);
  EXPECT_TRUE(DecodeSentMessages<dbapp::WriteEntity>(channel).empty());
  dbapp::CheckoutEntityAck reply;
  reply.blob = DatabaseBlob();
  reply.operation_id[0] = std::byte{1};
  reply.request_id = unrelated_id;
  reply.status = dbapp::CheckoutStatus::kDbError;
  handle_checkout_ack(source, reply);
  EXPECT_FALSE(unrelated->IsPendingDestroy());
  reply.request_id = outgoing[0].request_id;
  handle_checkout_ack(source, reply);
  EXPECT_TRUE(native_checkout_contains(reply.request_id));
  reply.operation_id = outgoing[0].operation_id;
  reply.error_kind = DatabaseErrorKind::kCommitUnknown;
  handle_checkout_ack(source, reply);
  EXPECT_TRUE(native_checkout_contains(reply.request_id));
  EXPECT_TRUE(active_login_contains(42));
  retry_native_now(reply.request_id);
  const auto retries = DecodeSentMessages<dbapp::CheckoutEntity>(channel);
  ASSERT_EQ(retries.size(), 2u);
  EXPECT_EQ(retries[0].operation_id, retries[1].operation_id);
  reply.error_kind = DatabaseErrorKind::kNone;
  handle_checkout_ack(source, reply);
  EXPECT_FALSE(native_checkout_contains(reply.request_id));
  EXPECT_TRUE(canceled_checkout_contains(reply.request_id));
  EXPECT_TRUE(active_login_contains(42));
  const auto aborts = DecodeSentMessages<dbapp::AbortCheckout>(channel);
  ASSERT_EQ(aborts.size(), 1u);
  EXPECT_EQ(aborts[0].operation_id, outgoing[0].operation_id);
  EXPECT_EQ(aborts[0].entity_id, outgoing[0].entity_id);
}

class FakeDatabase final : public IDatabase {
 public:
  Result<void> Startup(const DatabaseConfig&, const EntityDefRegistry&) override { return {}; }
  void Shutdown() override {}
  void BeginDrain() override {}
  std::size_t pending_operations{0};
  auto PendingOperations() const -> std::size_t override { return pending_operations; }

  void ReadAccountSession(DatabaseID, uint16_t,
                          std::function<void(Result<AccountSession>)> callback) override {
    ++session_reads;
    session_callback = std::move(callback);
  }
  void UpdateAccountSession(const AccountSessionOperation& operation,
                            std::function<void(Result<AccountSession>)> callback) override {
    ++session_updates;
    session_operation = operation;
    session_callback = std::move(callback);
  }
  int session_reads{0};
  int session_updates{0};
  AccountSessionOperation session_operation;
  std::function<void(Result<AccountSession>)> session_callback;

  void PutEntity(DatabaseID dbid, uint16_t type_id, WriteFlags flags, std::span<const std::byte>,
                 const std::string& identifier, std::function<void(PutResult)> callback) override {
    ++put_entity_calls;
    last_put_dbid = dbid;
    last_put_type_id = type_id;
    last_put_flags = flags;
    last_put_identifier = identifier;
    if (!complete_put_immediately) {
      put_callback = std::move(callback);
      return;
    }
    PutResult result;
    if (duplicate_dbid_failures_before_success > 0) {
      --duplicate_dbid_failures_before_success;
      result.dbid = dbid;
      result.error = "duplicate dbid";
      result.error_kind = DatabaseErrorKind::kDuplicateDbid;
      callback(std::move(result));
      return;
    }
    result.success = put_success;
    result.dbid = dbid != kInvalidDBID ? dbid : fallback_put_dbid;
    result.error = put_error;
    callback(std::move(result));
  }

  void PutEntityWithPassword(DatabaseID dbid, uint16_t type_id, WriteFlags flags,
                             std::span<const std::byte>, const std::string& identifier,
                             const std::string& password_hash,
                             std::function<void(PutResult)> callback) override {
    ++put_entity_with_password_calls;
    last_put_dbid = dbid;
    last_put_type_id = type_id;
    last_put_flags = flags;
    last_put_identifier = identifier;
    last_password_hash = password_hash;
    if (!complete_put_immediately) {
      put_callback = std::move(callback);
      return;
    }
    PutResult result;
    if (duplicate_dbid_failures_before_success > 0) {
      --duplicate_dbid_failures_before_success;
      result.dbid = dbid;
      result.error = "duplicate dbid";
      result.error_kind = DatabaseErrorKind::kDuplicateDbid;
      callback(std::move(result));
      return;
    }
    result.success = put_success;
    result.dbid = dbid != kInvalidDBID ? dbid : fallback_put_dbid;
    result.error = put_error;
    callback(std::move(result));
  }

  void PutEntityWithOperation(const DatabaseWriteOperation& operation, DatabaseID dbid,
                              uint16_t type_id, WriteFlags flags, std::span<const std::byte> blob,
                              const std::string& identifier,
                              std::function<void(PutResult)> callback) override {
    last_operation = operation;
    PutEntity(dbid, type_id, flags, blob, identifier, std::move(callback));
  }

  void GetEntity(DatabaseID, uint16_t, std::function<void(GetResult)> callback) override {
    ++get_entity_calls;
    callback(readback);
  }
  int get_entity_calls{0};
  GetResult readback{false, {}, {}, "injected readback unavailable"};

  void DelEntity(DatabaseID, uint16_t, std::function<void(DelResult)>) override {
    ADD_FAILURE() << "DelEntity should not be called in this test";
  }

  void LookupByName(uint16_t type_id, const std::string& identifier,
                    std::function<void(LookupResult)> callback) override {
    ++lookup_by_name_calls;
    last_lookup_type_id = type_id;
    last_lookup_identifier = identifier;
    if (defer_name_lookups) {
      name_lookup_callbacks.push_back(std::move(callback));
      return;
    }
    LookupResult result;
    result.found = lookup_found;
    result.dbid = lookup_dbid;
    result.password_hash = lookup_password_hash;
    result.error = lookup_error;
    callback(std::move(result));
  }

  void CheckoutEntity(DatabaseID, uint16_t, const CheckoutInfo&,
                      std::function<void(GetResult)> callback) override {
    checkout_callback = std::move(callback);
  }

  void CheckoutEntityByName(uint16_t, const std::string&, const CheckoutInfo&,
                            std::function<void(GetResult)> callback) override {
    checkout_callback = std::move(callback);
  }

  void CheckoutWithOperation(const DatabaseCheckoutOperation& operation,
                             std::function<void(GetResult)> callback) override {
    last_checkout_operation = operation;
    checkout_callback = std::move(callback);
  }

  void ResolveCheckoutOperation(const DatabaseCheckoutOperation& operation,
                                std::function<void(GetResult)> callback) override {
    if (operation.id != last_checkout_operation.id ||
        operation.dbid != last_checkout_operation.dbid ||
        operation.type_id != last_checkout_operation.type_id ||
        operation.identifier != last_checkout_operation.identifier ||
        operation.owner != last_checkout_operation.owner) {
      GetResult result;
      result.error_kind = DatabaseErrorKind::kOperationConflict;
      callback(std::move(result));
      return;
    }
    if (resolved_checkout)
      callback(*resolved_checkout);
    else if (checkout_callback)
      resolve_callback = std::move(callback);
    else {
      GetResult result;
      result.error_kind = DatabaseErrorKind::kCommitUnknown;
      callback(std::move(result));
    }
  }
  std::optional<GetResult> resolved_checkout;
  std::function<void(GetResult)> resolve_callback;

  auto SupportsMultiDbapp() const -> bool override { return shared; }
  void ReleaseCheckout(DatabaseID, uint16_t, const CheckoutInfo& owner,
                       std::function<void(Result<void>)> callback) override {
    ++release_calls;
    last_release_owner = owner;
    if (!complete_release_immediately) {
      release_callback = std::move(callback);
      return;
    }
    if (release_success)
      callback(Result<void>{});
    else
      callback(Error{ErrorCode::kPermissionDenied, "injected owner mismatch"});
  }
  bool shared{false};
  bool complete_release_immediately{true};
  std::function<void(Result<void>)> release_callback;
  bool release_success{true};
  int release_calls{0};
  CheckoutInfo last_release_owner{};

  void ClearCheckout(DatabaseID dbid, uint16_t type_id,
                     std::function<void(Result<bool>)> callback) override {
    ++clear_checkout_calls;
    last_cleared = std::make_pair(dbid, type_id);
    callback(true);
  }

  void ClearCheckoutsForAddress(const Address&,
                                std::function<void(Result<int>)> callback) override {
    callback(0);
  }

  void RetireCheckoutProcess(const Address& address, uint64_t instance,
                             std::function<void(Result<int>)> callback) override {
    ++retire_calls;
    retired_address = address;
    retired_instance = instance;
    retire_callback = std::move(callback);
  }
  int retire_calls{0};
  Address retired_address;
  uint64_t retired_instance{0};
  std::function<void(Result<int>)> retire_callback;

  void MarkCheckoutCleared(DatabaseID dbid, uint16_t type_id,
                           std::function<void(Result<void>)> callback) override {
    ++mark_checkout_cleared_calls;
    last_cleared = std::make_pair(dbid, type_id);
    callback({});
  }

  void GetAutoLoadEntities(std::function<void(Result<std::vector<EntityData>>)> callback) override {
    callback(std::vector<EntityData>{});
  }

  void SetAutoLoad(DatabaseID, uint16_t, bool,
                   std::function<void(Result<void>)> callback) override {
    callback({});
  }
  void GetMaxDbidInRange(DatabaseID low, DatabaseID high,
                         std::function<void(DbidRangeResult)> callback) override {
    max_dbid_range_requests.push_back({low, high});
    if (!complete_range_query_immediately) {
      range_query_callback = std::move(callback);
      return;
    }
    DbidRangeResult result;
    result.success = range_query_success;
    result.max_dbid = range_max_dbid;
    result.error = range_query_error;
    callback(std::move(result));
  }
  void ReserveEntityIds(uint32_t, std::function<void(Result<EntityIdRange>)> callback) override {
    callback(Error{ErrorCode::kNotSupported, "fixture does not allocate EntityIDs"});
  }
  void ProcessResults() override {}

  std::function<void(GetResult)> checkout_callback;
  DatabaseCheckoutOperation last_checkout_operation;
  int put_entity_calls{0};
  DatabaseWriteOperation last_operation;
  int put_entity_with_password_calls{0};
  DatabaseID last_put_dbid{kInvalidDBID};
  uint16_t last_put_type_id{0};
  WriteFlags last_put_flags{WriteFlags::kNone};
  std::string last_put_identifier;
  std::string last_password_hash;
  bool complete_put_immediately{true};
  std::function<void(PutResult)> put_callback;
  bool put_success{true};
  DatabaseID fallback_put_dbid{42};
  std::string put_error;
  int duplicate_dbid_failures_before_success{0};
  int lookup_by_name_calls{0};
  bool defer_name_lookups{false};
  std::vector<std::function<void(LookupResult)>> name_lookup_callbacks;
  uint16_t last_lookup_type_id{0};
  std::string last_lookup_identifier;
  bool lookup_found{false};
  DatabaseID lookup_dbid{kInvalidDBID};
  std::string lookup_password_hash;
  std::string lookup_error;
  std::vector<std::pair<DatabaseID, DatabaseID>> max_dbid_range_requests;
  bool complete_range_query_immediately{true};
  bool range_query_success{true};
  DatabaseID range_max_dbid{kInvalidDBID};
  std::string range_query_error;
  std::function<void(DbidRangeResult)> range_query_callback;
  int clear_checkout_calls{0};
  int mark_checkout_cleared_calls{0};
  std::optional<std::pair<DatabaseID, uint16_t>> last_cleared;
};

class DBAppRollbackTest : public ::testing::Test {
 protected:
  DBAppRollbackTest()
      : dispatcher_("dbapp_rollback"), network_(dispatcher_), app_(dispatcher_, network_) {
    app_.entity_defs_.emplace();
    app_.database_ = std::make_unique<FakeDatabase>();
  }

  auto db() -> FakeDatabase& { return *static_cast<FakeDatabase*>(app_.database_.get()); }

  void trust_session_peer(const Address& address, uint64_t instance) {
    app_.baseapp_instances_[address] = instance;
    app_.account_type_id_ = 7;
  }
  void account_session(Channel& channel, const dbapp::AccountSessionRequest& request) {
    app_.OnAccountSession(channel.RemoteAddress(), &channel, request);
  }
  void set_session_queue_full(bool full) {
    app_.account_session_requests_ = full ? app_.kMaxAccountSessionRequests : 0;
  }
  auto session_queue_size() const -> std::size_t { return app_.account_session_requests_; }

  void register_watchers() { app_.RegisterWatchers(); }
  void checkout_from(const Address& src, Channel* channel, const dbapp::CheckoutEntity& request) {
    app_.OnCheckoutEntity(src, channel, request);
  }

  void retire_process(const Address& address, uint64_t instance) {
    app_.OnBaseappDeath(address, instance);
  }
  void retry_retirement(const Address& address, uint64_t instance) {
    app_.RetireBaseappProcess(address, instance);
  }
  auto retirement_count() const -> std::size_t { return app_.process_retirements_.size(); }
  void complete_retirement(Result<int> result) {
    ASSERT_TRUE(db().retire_callback);
    auto callback = std::move(db().retire_callback);
    callback(std::move(result));
  }

  void start_checkout(uint32_t request_id, DatabaseID dbid, uint16_t type_id) {
    dbapp::CheckoutEntity checkout;
    checkout.owner_process_instance = 111;
    checkout.operation_id[0] = std::byte{1};
    checkout.request_id = request_id;
    checkout.dbid = dbid;
    checkout.type_id = type_id;
    app_.OnCheckoutEntity(Address("127.0.0.1", 30001), reinterpret_cast<Channel*>(1), checkout);
  }

  void write_entity(Channel* ch, const dbapp::WriteEntity& write) {
    app_.OnWriteEntity(Address("127.0.0.1", 30001), ch, write);
  }

  void auth_login(Channel* ch, const login::AuthLogin& auth,
                  const Address& src = Address("127.0.0.1", 30001), bool wait_for_password = true) {
    app_.OnAuthLogin(src, ch, auth);
    if (wait_for_password) drain_authentication_tasks();
  }

  void drain_authentication_tasks() {
    dispatcher_.SetMaxPollWait(Milliseconds(1));
    // Argon2 completion is the contract; VM scheduling is not a three-second latency guarantee.
    const auto deadline = Clock::now() + Seconds(30);
    while (app_.authentication_tasks_ && app_.authentication_tasks_->InFlightCount() != 0 &&
           Clock::now() < deadline)
      dispatcher_.ProcessOnce();
    ASSERT_TRUE(!app_.authentication_tasks_ || app_.authentication_tasks_->InFlightCount() == 0);
  }
  auto active_authentications() const -> std::size_t { return app_.active_authentications_; }
  auto queued_authentications() const -> std::size_t {
    return app_.pending_authentications_.size();
  }
  void stop_authentication() { app_.Shutdown(); }

  void checkout_entity(Channel* ch, const dbapp::CheckoutEntity& checkout) {
    app_.OnCheckoutEntity(Address("127.0.0.1", 30001), ch, checkout);
  }

  void checkin_entity(const dbapp::CheckinEntity& msg) {
    app_.OnCheckinEntity(Address("127.0.0.1", 30001), nullptr, msg);
  }

  void delete_entity(Channel* ch, const dbapp::DeleteEntity& msg) {
    app_.OnDeleteEntity(Address("127.0.0.1", 30001), ch, msg);
  }

  void abort_checkout(uint32_t request_id, DatabaseID dbid, uint16_t type_id,
                      std::byte operation_byte = std::byte{1}) {
    dbapp::AbortCheckout abort;
    abort.owner_process_instance = 111;
    abort.operation_id[0] = operation_byte;
    abort.request_id = request_id;
    abort.dbid = dbid;
    abort.type_id = type_id;
    abort.mode = db().last_checkout_operation.type_id != 0 &&
                         db().last_checkout_operation.dbid == kInvalidDBID
                     ? dbapp::LoadMode::kByName
                     : dbapp::LoadMode::kByDbid;
    abort.identifier = db().last_checkout_operation.identifier;
    abort.entity_id = db().last_checkout_operation.owner.entity_id;
    abort.owner_app_id = db().last_checkout_operation.owner.app_id;
    abort.owner_process_instance = db().last_checkout_operation.owner.process_instance;
    app_.OnAbortCheckout(Address("127.0.0.1", 30001), reinterpret_cast<Channel*>(1), abort);
  }

  void complete_checkout_success(DatabaseID dbid, uint64_t version = 1) {
    ASSERT_TRUE(db().checkout_callback);
    GetResult result;
    result.success = true;
    result.data.dbid = dbid;
    result.checkout_version = version;
    db().resolved_checkout = result;
    auto complete = std::move(db().checkout_callback);
    complete(result);
    if (db().resolve_callback) {
      auto resolve = std::move(db().resolve_callback);
      resolve(std::move(result));
    }
  }

  auto pending_request_contains(uint32_t request_id) const -> bool {
    return app_.pending_checkout_requests_.contains(request_key(request_id));
  }

  auto pending_request_canceled(uint32_t request_id) const -> bool {
    return app_.pending_checkout_requests_.at(request_key(request_id)).canceled;
  }

  auto cached_checkout(uint32_t request_id) const -> const dbapp::CheckoutEntityAck& {
    return app_.checkout_ack_cache_.at(request_key(request_id)).ack;
  }

  auto write_pending(const dbapp::WriteEntity& write) const -> bool {
    return app_.pending_write_requests_.contains(
        DBApp::RequestCacheKey{Address("127.0.0.1", 30001), write.request_id, write.operation_id});
  }

  auto checkout_owner(DatabaseID dbid, uint16_t type_id) const -> std::optional<CheckoutInfo> {
    return app_.checkout_mgr_.GetOwner(dbid, type_id);
  }

  void replace_checkout_owner(DatabaseID dbid, uint16_t type_id, const CheckoutInfo& owner) {
    app_.checkout_mgr_.Checkin(dbid, type_id);
    (void)app_.checkout_mgr_.TryCheckout(dbid, type_id, owner);
    app_.checkout_mgr_.ConfirmCheckout(dbid, type_id, owner.ownership_version);
  }

  auto abort_total() const -> uint64_t { return app_.abort_checkout_total_; }
  auto abort_pending_total() const -> uint64_t { return app_.abort_checkout_pending_hit_total_; }
  auto abort_late_total() const -> uint64_t { return app_.abort_checkout_late_hit_total_; }
  auto dbapp_id() const -> uint32_t { return app_.dbapp_id_; }
  auto shard_table_version() const -> uint32_t { return app_.shard_table_version_; }
  void set_shard_table_version(uint32_t version) { app_.shard_table_version_ = version; }
  void set_auto_create_accounts(bool enabled, uint16_t account_type_id) {
    app_.auto_create_accounts_ = enabled;
    app_.account_type_id_ = account_type_id;
  }
  auto shard_count() const -> std::size_t { return app_.shard_table_.size(); }
  auto shard_app_id(std::size_t index) const -> uint32_t {
    return app_.shard_table_.at(index).dbapp_id;
  }
  void handle_register_dbapp_ack(const dbappmgr::RegisterDbAppAck& ack) {
    app_.OnRegisterDbAppAck(Address(0x7F000001u, 28001), nullptr, ack);
  }
  void handle_shard_table_update(const dbappmgr::ShardTableUpdate& update) {
    app_.OnShardTableUpdate(Address(0x7F000001u, 28001), nullptr, update);
  }
  auto watcher(std::string_view path) -> std::optional<std::string> {
    return app_.GetWatcherRegistry().Get(path);
  }

  auto request_key(uint32_t request_id) const -> DBApp::RequestCacheKey {
    return DBApp::RequestCacheKey{Address("127.0.0.1", 30001), request_id, {std::byte{1}}};
  }

  EventDispatcher dispatcher_;
  NetworkInterface network_;
  DBApp app_;
};

TEST_F(DBAppRollbackTest, AccountSessionRejectsUnregisteredAndReplacedProcess) {
  InterfaceTable table;
  const Address source("127.0.0.1", 30001);
  auto channel = make_intrusive<CapturingChannel>(dispatcher_, table, source);
  dbapp::AccountSessionRequest request;
  request.request_id = (uint64_t{1} << 40) + 1;
  request.process_instance = 111;
  request.account_dbid = 42;
  request.account_type_id = 7;
  account_session(*channel, request);
  trust_session_peer(source, 222);
  account_session(*channel, request);

  EXPECT_EQ(db().session_reads, 0);
  const auto replies = DecodeSentMessages<dbapp::AccountSessionReply>(*channel);
  ASSERT_EQ(replies.size(), 2u);
  EXPECT_EQ(replies[0].request_id, request.request_id);
  EXPECT_EQ(replies[0].requester_instance, request.process_instance);
  for (const auto& reply : replies) EXPECT_EQ(reply.status, ErrorCode::kPermissionDenied);
}

TEST_F(DBAppRollbackTest, AccountSessionRepliesOnlyAfterBackendCompletion) {
  InterfaceTable table;
  const Address source("127.0.0.1", 30001);
  auto channel = make_intrusive<CapturingChannel>(dispatcher_, table, source);
  trust_session_peer(source, 111);
  dbapp::AccountSessionRequest request;
  request.request_id = (uint64_t{1} << 40) + 1;
  request.process_instance = 111;
  request.account_dbid = 42;
  request.account_type_id = 7;
  account_session(*channel, request);
  EXPECT_EQ(db().session_reads, 1);
  EXPECT_EQ(channel->send_count(), 0u);
  EXPECT_EQ(session_queue_size(), 1u);
  ASSERT_TRUE(db().session_callback);
  auto complete = std::move(db().session_callback);
  complete(Error{ErrorCode::kCommitUnknown, "injected commit uncertainty"});

  EXPECT_EQ(session_queue_size(), 0u);
  const auto replies = DecodeSentMessages<dbapp::AccountSessionReply>(*channel);
  ASSERT_EQ(replies.size(), 1u);
  EXPECT_EQ(replies[0].status, ErrorCode::kCommitUnknown);
  EXPECT_EQ(replies[0].request_id, request.request_id);
}

TEST_F(DBAppRollbackTest, AccountSessionRejectsWrongAccountTypeAndShard) {
  InterfaceTable table;
  const Address source("127.0.0.1", 30001);
  auto channel = make_intrusive<CapturingChannel>(dispatcher_, table, source);
  trust_session_peer(source, 111);
  dbapp::AccountSessionRequest request;
  request.request_id = 1;
  request.process_instance = 111;
  request.account_dbid = 42;
  request.account_type_id = 8;
  account_session(*channel, request);
  request.account_type_id = 7;
  dbappmgr::RegisterDbAppAck ack;
  ack.success = true;
  ack.dbapp_id = 7;
  ack.shard_table_version = 3;
  ack.entries.push_back({1, 100, 8, Address("127.0.0.1", 24002), false});
  handle_register_dbapp_ack(ack);
  request.shard_table_version = 2;
  account_session(*channel, request);
  request.shard_table_version = 3;
  account_session(*channel, request);

  EXPECT_EQ(db().session_reads, 0);
  EXPECT_EQ(session_queue_size(), 0u);
  const auto replies = DecodeSentMessages<dbapp::AccountSessionReply>(*channel);
  ASSERT_EQ(replies.size(), 3u);
  EXPECT_EQ(replies[0].status, ErrorCode::kInvalidArgument);
  EXPECT_EQ(replies[1].error, dbapp::kInvalidShardTableError);
  EXPECT_EQ(replies[2].status, ErrorCode::kPermissionDenied);
}

TEST_F(DBAppRollbackTest, AccountSessionDisconnectWhileWaitingReleasesPendingSlot) {
  InterfaceTable table;
  const Address source("127.0.0.1", 30001);
  auto channel = make_intrusive<CapturingChannel>(dispatcher_, table, source);
  trust_session_peer(source, 111);
  dbapp::AccountSessionRequest request;
  request.request_id = 1;
  request.process_instance = 111;
  request.account_dbid = 42;
  request.account_type_id = 7;
  account_session(*channel, request);
  ASSERT_EQ(session_queue_size(), 1u);
  ASSERT_TRUE(db().session_callback);
  channel->Condemn();
  auto complete = std::move(db().session_callback);
  complete(AccountSession{});
  EXPECT_EQ(session_queue_size(), 0u);
  EXPECT_EQ(channel->send_count(), 0u);
}

TEST_F(DBAppRollbackTest, AccountSessionRejectsActorMismatchAndOverload) {
  InterfaceTable table;
  const Address source("127.0.0.1", 30001);
  auto channel = make_intrusive<CapturingChannel>(dispatcher_, table, source);
  trust_session_peer(source, 111);
  dbapp::AccountSessionRequest request;
  request.request_id = 1;
  request.process_instance = 111;
  request.account_dbid = 42;
  request.account_type_id = 7;
  AccountSessionOperation operation;
  operation.id[0] = std::byte{1};
  operation.account_dbid = 42;
  operation.account_type_id = 7;
  operation.action = AccountSessionAction::kRelease;
  operation.expected_version = 3;
  operation.actor = {Address("127.0.0.1", 30002), 9, 111};
  request.operation = operation;
  account_session(*channel, request);
  request.operation->actor.base_addr = source;
  request.operation->actor.process_instance = 222;
  account_session(*channel, request);
  request.operation->actor.process_instance = 111;
  set_session_queue_full(true);
  account_session(*channel, request);
  set_session_queue_full(false);

  EXPECT_EQ(db().session_updates, 0);
  const auto replies = DecodeSentMessages<dbapp::AccountSessionReply>(*channel);
  ASSERT_EQ(replies.size(), 3u);
  EXPECT_EQ(replies[0].status, ErrorCode::kPermissionDenied);
  EXPECT_EQ(replies[1].status, ErrorCode::kPermissionDenied);
  EXPECT_EQ(replies[2].status, ErrorCode::kRateLimited);
}

TEST_F(DBAppRollbackTest, AccountSessionNormalizesActorAndChecksTargetInstance) {
  InterfaceTable table;
  const Address source("127.0.0.1", 30001);
  const Address target("127.0.0.1", 30002);
  auto channel = make_intrusive<CapturingChannel>(dispatcher_, table, source);
  trust_session_peer(source, 111);
  dbapp::AccountSessionRequest request;
  request.request_id = 1;
  request.process_instance = 111;
  request.account_dbid = 42;
  request.account_type_id = 7;
  AccountSessionOperation operation;
  operation.id[0] = std::byte{1};
  operation.account_dbid = 42;
  operation.account_type_id = 7;
  operation.action = AccountSessionAction::kPrepare;
  operation.expected_version = 3;
  operation.actor = {Address(0, source.Port()), 9, 111};
  operation.target = {target, 10, 222};
  operation.handoff_id = operation.id;
  operation.credential_hash[0] = std::byte{1};
  request.operation = operation;
  account_session(*channel, request);
  EXPECT_EQ(db().session_updates, 0);
  trust_session_peer(target, 222);
  account_session(*channel, request);
  EXPECT_EQ(db().session_updates, 1);
  EXPECT_EQ(db().session_operation.actor.base_addr, source);
  ASSERT_TRUE(db().session_callback);
  auto complete = std::move(db().session_callback);
  complete(Error{ErrorCode::kIoError, "injected database failure"});
  EXPECT_EQ(session_queue_size(), 0u);
}

TEST_F(DBAppRollbackTest, SharedWriteRejectsOwnerFromDifferentChannel) {
  db().shared = true;
  InterfaceTable table;
  CapturingChannel reply(dispatcher_, table, Address(0x7F000001, 30001));
  dbapp::WriteEntity write;
  write.blob = DatabaseBlob();
  write.owner.process_instance = 111;
  write.operation_id[0] = std::byte{1};
  write.dbid = 42;
  write.type_id = 9;
  write.owner = CheckoutInfo{Address(0x7F000001, 30002), 1, 2, 12, 111};
  write_entity(&reply, write);
  EXPECT_EQ(db().put_entity_calls, 0);
}

TEST_F(DBAppRollbackTest, SingleDbappWriteRejectsOwnerFromDifferentChannel) {
  InterfaceTable table;
  CapturingChannel reply(dispatcher_, table, Address(0x7F000001, 30001));
  dbapp::WriteEntity write;
  write.blob = DatabaseBlob();
  write.owner.process_instance = 111;
  write.operation_id[0] = std::byte{1};
  write.dbid = 42;
  write.type_id = 9;
  write.owner = CheckoutInfo{Address(0x7F000001, 30002), 1, 2, 12, 111};
  write_entity(&reply, write);
  EXPECT_EQ(db().put_entity_calls, 0);
}

TEST_F(DBAppRollbackTest, ChangedInflightWriteIsRejectedWithoutRemovingOriginalWaiter) {
  db().complete_put_immediately = false;
  InterfaceTable table;
  CapturingChannel reply(dispatcher_, table, Address("127.0.0.1", 30001));
  dbapp::WriteEntity write;
  write.blob = DatabaseBlob();
  write.owner.process_instance = 111;
  write.operation_id[0] = std::byte{1};
  write.request_id = 1001;
  write.dbid = 42;
  write.type_id = 9;
  write.blob = DatabaseBlob({std::byte{1}});
  write_entity(&reply, write);
  ASSERT_TRUE(write_pending(write));
  auto changed = write;
  changed.blob = DatabaseBlob({std::byte{2}});
  write_entity(&reply, changed);
  ASSERT_TRUE(write_pending(write));
  EXPECT_EQ(db().put_entity_calls, 1);
  const auto acks = DecodeSentMessages<dbapp::WriteEntityAck>(reply);
  ASSERT_EQ(acks.size(), 1u);
  EXPECT_FALSE(acks[0].success);
  EXPECT_EQ(acks[0].error_kind, DatabaseErrorKind::kOperationConflict);
  write_entity(&reply, write);
  EXPECT_EQ(db().put_entity_calls, 1);
  ASSERT_TRUE(db().put_callback);
  db().put_callback(PutResult{true, 42, ""});
  EXPECT_FALSE(write_pending(write));
}

TEST_F(DBAppRollbackTest, MismatchedEntityDefinitionsRejectWriteAndCheckoutBeforeDatabaseAccess) {
  InterfaceTable table;
  CapturingChannel reply(dispatcher_, table, Address("127.0.0.1", 30001));
  dbapp::WriteEntity write;
  write.blob = DatabaseBlob();
  write.operation_id[0] = std::byte{1};
  write.owner.process_instance = 111;
  write.dbid = 42;
  write.type_id = 9;
  write.entity_def_digest[0] = 1;
  write_entity(&reply, write);
  EXPECT_EQ(db().put_entity_calls, 0);
  auto writes = DecodeSentMessages<dbapp::WriteEntityAck>(reply);
  ASSERT_EQ(writes.size(), 1u);
  EXPECT_FALSE(writes[0].success);
  EXPECT_EQ(writes[0].error, "entity_def_mismatch");

  dbapp::CheckoutEntity checkout;
  checkout.operation_id[0] = std::byte{1};
  checkout.owner_process_instance = 111;
  checkout.owner_app_id = 7;
  checkout.dbid = 42;
  checkout.type_id = 9;
  checkout.entity_def_digest[0] = 1;
  checkout_from(reply.RemoteAddress(), &reply, checkout);
  EXPECT_FALSE(db().checkout_callback);
  auto checkouts = DecodeSentMessages<dbapp::CheckoutEntityAck>(reply);
  ASSERT_EQ(checkouts.size(), 1u);
  EXPECT_EQ(checkouts[0].status, dbapp::CheckoutStatus::kDbError);
  EXPECT_EQ(checkouts[0].error, "entity_def_mismatch");
}

TEST_F(DBAppRollbackTest, MismatchedLoginDefinitionsDoNotQueryOrCreateAccounts) {
  InterfaceTable table;
  CapturingChannel reply(dispatcher_, table, Address("127.0.0.1", 30001));
  login::AuthLogin request;
  request.username = "schema-test";
  request.password_hash = "test-only-placeholder";
  request.auto_create = true;
  request.entity_def_digest[0] = 1;
  auth_login(&reply, request);
  EXPECT_EQ(db().lookup_by_name_calls, 0);
  EXPECT_EQ(db().put_entity_with_password_calls, 0);
  auto replies = DecodeSentMessages<login::AuthLoginResult>(reply);
  ASSERT_EQ(replies.size(), 1u);
  EXPECT_EQ(replies[0].status, login::LoginStatus::kDefMismatch);
}

TEST_F(DBAppRollbackTest, EmbeddedNullCredentialsDoNotQueryOrCreateAccounts) {
  set_auto_create_accounts(true, 1);
  for (bool null_username : {true, false}) {
    login::AuthLogin request;
    request.username = null_username ? std::string("a\0b", 3) : "account";
    request.password_hash = null_username ? "credential" : std::string("p\0q", 3);
    request.auto_create = true;
    auth_login(nullptr, request);
  }
  EXPECT_EQ(db().lookup_by_name_calls, 0);
  EXPECT_EQ(db().put_entity_with_password_calls, 0);
}

TEST_F(DBAppRollbackTest, SingleDbappLogoffForwardsLeaseAndKeepsNewOwnerOnLateCompletion) {
  start_checkout(1001, 555, 9);
  complete_checkout_success(555, 42);
  db().complete_put_immediately = false;
  dbapp::WriteEntity write;
  write.blob = DatabaseBlob();
  write.owner.process_instance = 111;
  write.operation_id[0] = std::byte{1};
  write.request_id = 1002;
  write.dbid = 555;
  write.type_id = 9;
  write.owner = *checkout_owner(555, 9);
  write.flags = WriteFlags::kLogOff;
  write_entity(test_support::FakeChannel(0x1505), write);
  ASSERT_TRUE(db().last_operation.expected_owner);
  EXPECT_EQ(db().last_operation.expected_owner->ownership_version, 42u);
  ASSERT_TRUE(db().put_callback);
  auto current = write.owner;
  current.ownership_version = 43;
  replace_checkout_owner(555, 9, current);
  db().put_callback(PutResult{true, 555, ""});
  ASSERT_TRUE(checkout_owner(555, 9));
  EXPECT_EQ(checkout_owner(555, 9)->ownership_version, 43u);
}

TEST_F(DBAppRollbackTest, SingleDbappDeleteRequiresSourceBoundVersionedOwner) {
  InterfaceTable table;
  CapturingChannel reply(dispatcher_, table, Address(0x7F000001, 30001));
  dbapp::DeleteEntity removal;
  removal.dbid = 555;
  removal.type_id = 9;
  removal.operation_id[0] = std::byte{1};
  delete_entity(&reply, removal);
  EXPECT_EQ(db().put_entity_calls, 0);
  removal.owner = CheckoutInfo{Address("127.0.0.1", 30002), 1, 2, 12, 111};
  delete_entity(&reply, removal);
  EXPECT_EQ(db().put_entity_calls, 0);
  removal.owner.base_addr = Address("127.0.0.1", 30001);
  delete_entity(&reply, removal);
  EXPECT_EQ(db().put_entity_calls, 1);
  EXPECT_TRUE(HasFlag(db().last_put_flags, WriteFlags::kDelete));
  ASSERT_TRUE(db().last_operation.expected_owner);
  EXPECT_EQ(db().last_operation.expected_owner->ownership_version, 12u);
}

TEST_F(DBAppRollbackTest, SharedNamedCheckoutStoresVersionUnderResolvedDbid) {
  db().shared = true;
  InterfaceTable table;
  CapturingChannel reply(dispatcher_, table, Address(0x7F000001, 30001));
  dbapp::CheckoutEntity checkout;
  checkout.owner_process_instance = 111;
  checkout.operation_id[0] = std::byte{1};
  checkout.mode = dbapp::LoadMode::kByName;
  checkout.type_id = 9;
  checkout.request_id = 1001;
  checkout.identifier = "named";
  checkout_entity(&reply, checkout);
  complete_checkout_success(555, 42);
  EXPECT_FALSE(checkout_owner(kInvalidDBID, 9).has_value());
  ASSERT_TRUE(checkout_owner(555, 9).has_value());
  EXPECT_EQ(checkout_owner(555, 9)->ownership_version, 42u);
}

TEST_F(DBAppRollbackTest, SharedCheckoutRechecksStaleConfirmedMemory) {
  db().shared = true;
  start_checkout(1001, 555, 9);
  complete_checkout_success(555, 42);
  ASSERT_TRUE(checkout_owner(555, 9).has_value());
  start_checkout(1002, 555, 9);
  EXPECT_TRUE(pending_request_contains(1002));
  complete_checkout_success(555, 43);
  ASSERT_TRUE(checkout_owner(555, 9).has_value());
  EXPECT_EQ(checkout_owner(555, 9)->ownership_version, 43u);
}

TEST_F(DBAppRollbackTest, SharedAbortWaitsForActualAcquiredVersion) {
  db().shared = true;
  start_checkout(1001, 555, 9);
  abort_checkout(1001, 555, 9);
  EXPECT_EQ(db().release_calls, 0);
  EXPECT_EQ(db().mark_checkout_cleared_calls, 0);
  complete_checkout_success(555, 42);
  EXPECT_EQ(db().release_calls, 1);
  EXPECT_EQ(db().last_release_owner.ownership_version, 42u);
  EXPECT_FALSE(checkout_owner(555, 9).has_value());
}

TEST_F(DBAppRollbackTest, SharedUnknownAbortNeverClearsCheckoutByDbid) {
  db().shared = true;
  start_checkout(1001, 555, 9);
  complete_checkout_success(555, 42);
  abort_checkout(1002, 555, 9, std::byte{2});
  EXPECT_EQ(db().release_calls, 0);
  EXPECT_EQ(db().mark_checkout_cleared_calls, 0);
  ASSERT_TRUE(checkout_owner(555, 9).has_value());
  EXPECT_EQ(checkout_owner(555, 9)->ownership_version, 42u);
}

TEST_F(DBAppRollbackTest, SingleDbappCheckinRetainsOwnerUntilDatabaseConfirms) {
  db().shared = false;
  start_checkout(1001, 555, 9);
  complete_checkout_success(555, 42);
  dbapp::CheckinEntity checkin;
  checkin.dbid = 555;
  checkin.type_id = 9;
  checkin.owner = *checkout_owner(555, 9);
  db().release_success = false;
  checkin_entity(checkin);
  EXPECT_TRUE(checkout_owner(555, 9).has_value());
  db().release_success = true;
  checkin_entity(checkin);
  EXPECT_FALSE(checkout_owner(555, 9).has_value());
  EXPECT_EQ(db().mark_checkout_cleared_calls, 0);
}

TEST_F(DBAppRollbackTest, SingleDbappRejectsUnknownAbortAndWrongSourceCheckin) {
  start_checkout(1001, 555, 9);
  complete_checkout_success(555, 42);
  abort_checkout(1002, 555, 9, std::byte{2});
  EXPECT_EQ(db().release_calls, 0);
  ASSERT_TRUE(checkout_owner(555, 9));
  dbapp::CheckinEntity msg;
  msg.dbid = 555;
  msg.type_id = 9;
  msg.owner = *checkout_owner(555, 9);
  msg.owner.base_addr = Address("127.0.0.1", 30002);
  checkin_entity(msg);
  EXPECT_EQ(db().release_calls, 0);
  EXPECT_TRUE(checkout_owner(555, 9));
  EXPECT_EQ(db().mark_checkout_cleared_calls, 0);
}

TEST_F(DBAppRollbackTest, SingleDbappCancelKeepsPendingOwnerUntilReleaseCompletes) {
  db().complete_release_immediately = false;
  start_checkout(1001, 555, 9);
  abort_checkout(1001, 555, 9);
  EXPECT_EQ(db().release_calls, 0);
  complete_checkout_success(555, 42);
  ASSERT_TRUE(db().release_callback);
  EXPECT_EQ(db().last_release_owner.ownership_version, 42u);
  EXPECT_TRUE(checkout_owner(555, 9));
  db().release_callback(Result<void>{});
  EXPECT_FALSE(checkout_owner(555, 9));
  EXPECT_EQ(db().mark_checkout_cleared_calls, 0);
}

TEST_F(DBAppRollbackTest, SingleDbappCancelCanRetryFailedReleaseWithAcquiredVersion) {
  db().release_success = false;
  start_checkout(1001, 555, 9);
  abort_checkout(1001, 555, 9);
  complete_checkout_success(555, 42);
  ASSERT_TRUE(checkout_owner(555, 9));
  EXPECT_EQ(checkout_owner(555, 9)->ownership_version, 42u);
  EXPECT_EQ(db().release_calls, 1);
  db().release_success = true;
  abort_checkout(1001, 555, 9);
  EXPECT_EQ(db().release_calls, 2);
  EXPECT_EQ(db().last_release_owner.ownership_version, 42u);
  EXPECT_FALSE(checkout_owner(555, 9));
}

TEST_F(DBAppRollbackTest, LateCheckinCompletionKeepsNewerLease) {
  start_checkout(1001, 555, 9);
  complete_checkout_success(555, 42);
  dbapp::CheckinEntity msg;
  msg.dbid = 555;
  msg.type_id = 9;
  msg.owner = *checkout_owner(555, 9);
  db().complete_release_immediately = false;
  checkin_entity(msg);
  ASSERT_TRUE(db().release_callback);
  auto new_owner = msg.owner;
  new_owner.ownership_version = 43;
  replace_checkout_owner(555, 9, new_owner);
  db().release_callback(Result<void>{});
  ASSERT_TRUE(checkout_owner(555, 9));
  EXPECT_EQ(checkout_owner(555, 9)->ownership_version, 43u);
}

TEST_F(DBAppRollbackTest, PendingAbortRejectsMismatchedTarget) {
  start_checkout(1001, 555, 9);
  abort_checkout(1001, 556, 9);
  EXPECT_FALSE(pending_request_canceled(1001));
  abort_checkout(1001, 555, 10);
  EXPECT_FALSE(pending_request_canceled(1001));
  complete_checkout_success(555, 42);
  EXPECT_EQ(db().release_calls, 0);
  EXPECT_EQ(cached_checkout(1001).status, dbapp::CheckoutStatus::kSuccess);
}

TEST_F(DBAppRollbackTest, CompletedAbortRejectsMismatchedType) {
  start_checkout(1001, 555, 9);
  complete_checkout_success(555, 42);
  abort_checkout(1001, 555, 10);
  EXPECT_EQ(db().release_calls, 0);
  ASSERT_TRUE(checkout_owner(555, 9));
  EXPECT_EQ(checkout_owner(555, 9)->ownership_version, 42u);
}

TEST_F(DBAppRollbackTest, LateNamedAbortReleasesResolvedDatabaseId) {
  InterfaceTable table;
  CapturingChannel reply(dispatcher_, table, Address(0x7F000001, 30001));
  dbapp::CheckoutEntity checkout;
  checkout.owner_process_instance = 111;
  checkout.operation_id[0] = std::byte{1};
  checkout.mode = dbapp::LoadMode::kByName;
  checkout.type_id = 9;
  checkout.request_id = 1001;
  checkout.identifier = "named";
  checkout_entity(&reply, checkout);
  complete_checkout_success(555, 42);
  abort_checkout(1001, kInvalidDBID, 9);
  EXPECT_EQ(db().release_calls, 1);
  EXPECT_EQ(db().last_release_owner.ownership_version, 42u);
  EXPECT_FALSE(checkout_owner(555, 9));
}

TEST_F(DBAppRollbackTest, CachedCheckoutRejectsChangedRequest) {
  start_checkout(1001, 555, 9);
  complete_checkout_success(555, 42);
  InterfaceTable table;
  CapturingChannel reply(dispatcher_, table, Address(0x7F000001, 30001));
  dbapp::CheckoutEntity checkout;
  checkout.owner_process_instance = 111;
  checkout.operation_id[0] = std::byte{1};
  checkout.request_id = 1001;
  checkout.dbid = 556;
  checkout.type_id = 9;
  checkout_entity(&reply, checkout);
  const auto acks = DecodeSentMessages<dbapp::CheckoutEntityAck>(reply);
  ASSERT_EQ(acks.size(), 1u);
  EXPECT_EQ(acks[0].status, dbapp::CheckoutStatus::kDbError);
  EXPECT_EQ(acks[0].error, "checkout_request_conflict");
  EXPECT_EQ(cached_checkout(1001).dbid, 555);
}

TEST_F(DBAppRollbackTest, PendingCheckoutRejectsChangedRequestWithoutCancelingOriginal) {
  InterfaceTable table;
  CapturingChannel reply(dispatcher_, table, Address("127.0.0.1", 30001));
  dbapp::CheckoutEntity checkout;
  checkout.owner_process_instance = 111;
  checkout.operation_id[0] = std::byte{1};
  checkout.request_id = 1001;
  checkout.dbid = 555;
  checkout.type_id = 9;
  checkout_entity(&reply, checkout);
  auto changed = checkout;
  changed.dbid = 556;
  checkout_entity(&reply, changed);
  const auto acks = DecodeSentMessages<dbapp::CheckoutEntityAck>(reply);
  ASSERT_EQ(acks.size(), 1u);
  EXPECT_EQ(acks[0].status, dbapp::CheckoutStatus::kDbError);
  EXPECT_EQ(acks[0].error, "checkout_request_conflict");
  EXPECT_TRUE(pending_request_contains(1001));
  EXPECT_FALSE(pending_request_canceled(1001));
  checkout_entity(&reply, checkout);
  EXPECT_EQ(DecodeSentMessages<dbapp::CheckoutEntityAck>(reply).size(), 1u);
  complete_checkout_success(555, 42);
  EXPECT_FALSE(pending_request_contains(1001));
  EXPECT_EQ(cached_checkout(1001).dbid, 555);
  EXPECT_EQ(cached_checkout(1001).checkout_version, 42u);
}

TEST_F(DBAppRollbackTest, CheckoutBindsStableOperationToSourceAndRejectsWrongCancellation) {
  InterfaceTable table;
  CapturingChannel reply(dispatcher_, table, Address("127.0.0.1", 30001));
  dbapp::CheckoutEntity checkout;
  checkout.owner_process_instance = 111;
  checkout.request_id = 1001;
  checkout.dbid = 555;
  checkout.type_id = 9;
  checkout.owner_app_id = 7;
  checkout.entity_id = 77;
  checkout.owner_addr = Address("127.0.0.1", 30002);
  checkout_entity(&reply, checkout);
  EXPECT_FALSE(pending_request_contains(1001));
  const auto replies = DecodeSentMessages<dbapp::CheckoutEntityAck>(reply);
  ASSERT_EQ(replies.size(), 1u);
  EXPECT_EQ(replies[0].status, dbapp::CheckoutStatus::kDbError);
  EXPECT_EQ(replies[0].error, "invalid_checkout_operation");
  checkout.operation_id[0] = std::byte{1};
  checkout_entity(&reply, checkout);
  ASSERT_TRUE(pending_request_contains(1001));
  EXPECT_EQ(db().last_checkout_operation.id, checkout.operation_id);
  EXPECT_EQ(db().last_checkout_operation.owner.base_addr, Address("127.0.0.1", 30001));
  EXPECT_EQ(db().last_checkout_operation.owner.app_id, 7u);
  EXPECT_EQ(db().last_checkout_operation.owner.entity_id, 77u);
  abort_checkout(1001, 555, 9, std::byte{2});
  EXPECT_FALSE(pending_request_canceled(1001));
  abort_checkout(1001, 555, 9);
  EXPECT_TRUE(pending_request_canceled(1001));
  complete_checkout_success(555, 42);
  EXPECT_EQ(db().release_calls, 1);
  EXPECT_EQ(db().last_release_owner.ownership_version, 42u);
}

TEST_F(DBAppRollbackTest, CheckoutNeverAcknowledgesZeroVersionAsSuccess) {
  NetworkInterface receiver(dispatcher_);
  ASSERT_TRUE(receiver.StartRudpServer(Address("127.0.0.1", 0)));
  auto channel = network_.ConnectRudpNocwnd(receiver.RudpAddress());
  ASSERT_TRUE(channel);
  std::optional<dbapp::CheckoutEntityAck> reply;
  ASSERT_TRUE(receiver.InterfaceTable().RegisterTypedHandler<dbapp::CheckoutEntityAck>(
      [&](const Address&, Channel*, const dbapp::CheckoutEntityAck& result) { reply = result; }));
  dbapp::CheckoutEntity request;
  request.owner_process_instance = 111;
  request.operation_id[0] = std::byte{1};
  request.request_id = 1001;
  request.dbid = 555;
  request.type_id = 9;
  checkout_from(receiver.RudpAddress(), *channel, request);
  complete_checkout_success(555, 0);
  dispatcher_.SetMaxPollWait(Milliseconds(1));
  const auto deadline = Clock::now() + Seconds(2);
  while (!reply && Clock::now() < deadline) dispatcher_.ProcessOnce();
  ASSERT_TRUE(reply);
  EXPECT_EQ(reply->status, dbapp::CheckoutStatus::kDbError);
  EXPECT_EQ(reply->error, "invalid_checkout_lease");
  EXPECT_EQ(reply->error_kind, DatabaseErrorKind::kCommitUnknown);
  EXPECT_FALSE(checkout_owner(555, 9));
  reply.reset();
  checkout_from(receiver.RudpAddress(), *channel, request);
  ASSERT_TRUE(db().checkout_callback);
  complete_checkout_success(555, 7);
  const auto retry_deadline = Clock::now() + Seconds(2);
  while (!reply && Clock::now() < retry_deadline) dispatcher_.ProcessOnce();
  ASSERT_TRUE(reply);
  EXPECT_EQ(reply->status, dbapp::CheckoutStatus::kSuccess);
  EXPECT_EQ(reply->checkout_version, 7u);
}

TEST_F(DBAppRollbackTest, AbortCheckoutIsIdempotentWhileRequestIsPending) {
  register_watchers();
  start_checkout(1001, 555, 9);
  ASSERT_TRUE(db().checkout_callback);
  ASSERT_TRUE(pending_request_contains(1001));

  abort_checkout(1001, 555, 9);
  abort_checkout(1001, 555, 9);

  EXPECT_EQ(db().mark_checkout_cleared_calls, 0);
  ASSERT_TRUE(pending_request_contains(1001));
  EXPECT_TRUE(pending_request_canceled(1001));

  EXPECT_EQ(db().release_calls, 0);
  complete_checkout_success(555);
  EXPECT_EQ(db().release_calls, 1);

  EXPECT_FALSE(pending_request_contains(1001));
  EXPECT_EQ(db().mark_checkout_cleared_calls, 0);
  EXPECT_FALSE(checkout_owner(555, 9).has_value());
  EXPECT_EQ(abort_total(), 2u);
  EXPECT_EQ(abort_pending_total(), 2u);
  EXPECT_EQ(abort_late_total(), 0u);
  EXPECT_EQ(watcher("dbapp/abort_checkout_total").value_or(""), "2");
}

TEST_F(DBAppRollbackTest, RegisterDbAppAckStoresShardState) {
  register_watchers();
  EXPECT_EQ(watcher("app/not_ready_reason"), "shards_unregistered");
  EXPECT_EQ(watcher("dbapp/pending_operations"), "0");

  dbappmgr::RegisterDbAppAck ack;
  ack.success = true;
  ack.dbapp_id = 7;
  ack.shard_table_version = 3;
  ack.entries.push_back(dbappmgr::ShardEntry{1, 1000, 7, Address(0x7F000001u, 24001), false});

  handle_register_dbapp_ack(ack);

  EXPECT_EQ(dbapp_id(), 7u);
  EXPECT_EQ(shard_table_version(), 3u);
  EXPECT_EQ(shard_count(), 1u);
  EXPECT_EQ(watcher("dbapp/dbapp_id").value_or(""), "7");
  EXPECT_EQ(watcher("dbapp/shard_table_version").value_or(""), "3");
  EXPECT_EQ(watcher("app/ready"), "true");
  const auto path_count = app_.GetWatcherRegistry().List().size();
  db().pending_operations = IDatabase::kMaxPendingOperations;
  EXPECT_EQ(watcher("app/ready"), "false");
  EXPECT_EQ(watcher("app/not_ready_reason"), "database_queue_full");
  EXPECT_EQ(watcher("dbapp/pending_operations"), "4096");
  EXPECT_EQ(app_.GetWatcherRegistry().List().size(), path_count);
  db().pending_operations = 0;
  EXPECT_EQ(watcher("app/ready"), "true");
}

TEST_F(DBAppRollbackTest, RegisterDbAppAckKeepsLocalShardStateForRecoveryAck) {
  dbappmgr::RegisterDbAppAck ack;
  ack.success = true;
  ack.dbapp_id = 7;
  ack.shard_table_version = 3;
  ack.entries.push_back(dbappmgr::ShardEntry{1, 1000, 7, Address(0x7F000001u, 24001), false});
  handle_register_dbapp_ack(ack);

  dbappmgr::RegisterDbAppAck recovering;
  recovering.success = true;
  recovering.dbapp_id = 7;
  handle_register_dbapp_ack(recovering);

  EXPECT_EQ(dbapp_id(), 7u);
  EXPECT_EQ(shard_table_version(), 3u);
  ASSERT_EQ(shard_count(), 1u);
  EXPECT_EQ(shard_app_id(0), 7u);
}

TEST_F(DBAppRollbackTest, ShardTableUpdateReplacesCachedTable) {
  dbappmgr::RegisterDbAppAck ack;
  ack.success = true;
  ack.dbapp_id = 7;
  ack.shard_table_version = 3;
  ack.entries.push_back(dbappmgr::ShardEntry{1, 1000, 7, Address(0x7F000001u, 24001), false});
  handle_register_dbapp_ack(ack);

  dbappmgr::ShardTableUpdate update;
  update.version = 4;
  update.entries.push_back(dbappmgr::ShardEntry{1, 500, 7, Address(0x7F000001u, 24001), false});
  update.entries.push_back(dbappmgr::ShardEntry{500, 1000, 8, Address(0x7F000001u, 24002), false});

  handle_shard_table_update(update);

  EXPECT_EQ(shard_table_version(), 4u);
  ASSERT_EQ(shard_count(), 2u);
  EXPECT_EQ(shard_app_id(1), 8u);
}

TEST_F(DBAppRollbackTest, ShardTableUpdateIgnoresStaleVersion) {
  dbappmgr::RegisterDbAppAck ack;
  ack.success = true;
  ack.dbapp_id = 7;
  ack.shard_table_version = 4;
  ack.entries.push_back(dbappmgr::ShardEntry{1, 1000, 7, Address(0x7F000001u, 24001), false});
  handle_register_dbapp_ack(ack);

  dbappmgr::ShardTableUpdate stale;
  stale.version = 3;
  stale.entries.push_back(dbappmgr::ShardEntry{1, 1000, 8, Address(0x7F000001u, 24002), false});
  handle_shard_table_update(stale);

  EXPECT_EQ(shard_table_version(), 4u);
  ASSERT_EQ(shard_count(), 1u);
  EXPECT_EQ(shard_app_id(0), 7u);
}

TEST_F(DBAppRollbackTest, WriteEntityCreateAllocatesExplicitDbidFromOwnedShard) {
  db().complete_put_immediately = false;
  dbappmgr::RegisterDbAppAck ack;
  ack.success = true;
  ack.dbapp_id = 7;
  ack.shard_table_version = 1;
  ack.entries.push_back(dbappmgr::ShardEntry{1000, 2000, 7, Address(0x7F000001u, 24001), false});
  handle_register_dbapp_ack(ack);

  dbapp::WriteEntity write;
  write.blob = DatabaseBlob();
  write.owner.process_instance = 111;
  write.operation_id[0] = std::byte{1};
  write.request_id = 77;
  write.entity_id = 9;
  write.type_id = 9;
  write.flags = WriteFlags::kCreateNew;
  write.shard_table_version = 1;
  write_entity(test_support::FakeChannel(0x1505), write);

  EXPECT_EQ(db().put_entity_calls, 1);
  EXPECT_EQ(db().last_put_dbid, 1000);
  EXPECT_TRUE(HasFlag(db().last_put_flags, WriteFlags::kCreateNew));
  EXPECT_TRUE(HasFlag(db().last_put_flags, WriteFlags::kExplicitDbid));
  ASSERT_TRUE(db().last_operation.create_owner);
  EXPECT_EQ(db().last_operation.create_owner->base_addr, Address("127.0.0.1", 30001));
  EXPECT_EQ(db().last_operation.create_owner->entity_id, write.entity_id);
}

TEST_F(DBAppRollbackTest, WriteEntityCreateResumesAfterStoredShardMaxDbid) {
  db().complete_put_immediately = false;
  db().range_max_dbid = 1234;

  dbappmgr::RegisterDbAppAck ack;
  ack.success = true;
  ack.dbapp_id = 7;
  ack.shard_table_version = 1;
  ack.entries.push_back(dbappmgr::ShardEntry{1000, 2000, 7, Address(0x7F000001u, 24001), false});
  handle_register_dbapp_ack(ack);

  ASSERT_EQ(db().max_dbid_range_requests.size(), 1u);
  EXPECT_EQ(db().max_dbid_range_requests[0].first, 1000);
  EXPECT_EQ(db().max_dbid_range_requests[0].second, 2000);

  dbapp::WriteEntity write;
  write.blob = DatabaseBlob();
  write.owner.process_instance = 111;
  write.operation_id[0] = std::byte{1};
  write.request_id = 78;
  write.entity_id = 9;
  write.type_id = 9;
  write.flags = WriteFlags::kCreateNew;
  write.shard_table_version = 1;
  write_entity(test_support::FakeChannel(0x1505), write);

  EXPECT_EQ(db().put_entity_calls, 1);
  EXPECT_EQ(db().last_put_dbid, 1235);
  EXPECT_TRUE(HasFlag(db().last_put_flags, WriteFlags::kCreateNew));
  EXPECT_TRUE(HasFlag(db().last_put_flags, WriteFlags::kExplicitDbid));
}

TEST_F(DBAppRollbackTest, WriteEntityCreateRetriesDuplicateDbidInsideOwnedShard) {
  db().duplicate_dbid_failures_before_success = 1;

  dbappmgr::RegisterDbAppAck ack;
  ack.success = true;
  ack.dbapp_id = 7;
  ack.shard_table_version = 1;
  ack.entries.push_back(dbappmgr::ShardEntry{1000, 2000, 7, Address(0x7F000001u, 24001), false});
  handle_register_dbapp_ack(ack);

  dbapp::WriteEntity write;
  write.blob = DatabaseBlob();
  write.owner.process_instance = 111;
  write.operation_id[0] = std::byte{1};
  write.request_id = 79;
  write.entity_id = 9;
  write.type_id = 9;
  write.flags = WriteFlags::kCreateNew;
  write.shard_table_version = 1;
  write_entity(test_support::FakeChannel(0x1505), write);

  EXPECT_EQ(db().put_entity_calls, 2);
  EXPECT_EQ(db().last_put_dbid, 1001);
  EXPECT_TRUE(HasFlag(db().last_put_flags, WriteFlags::kCreateNew));
  EXPECT_TRUE(HasFlag(db().last_put_flags, WriteFlags::kExplicitDbid));
}

TEST_F(DBAppRollbackTest, AutoCreateStoresServerDerivedPassword) {
  set_auto_create_accounts(true, 1);
  login::AuthLogin auth;
  auth.request_id = 87;
  auth.username = "derived_account";
  auth.password_hash = "test-only-credential";
  auth.auto_create = true;
  auth_login(test_support::FakeChannel(0x1506), auth);
  ASSERT_EQ(db().put_entity_with_password_calls, 1);
  EXPECT_NE(db().last_password_hash, auth.password_hash);
  EXPECT_TRUE(db().last_password_hash.starts_with("$argon2id$v=19$m=65536,t=3,p=4$"));
  auto valid = VerifyPassword(auth.password_hash, db().last_password_hash);
  ASSERT_TRUE(valid);
  EXPECT_TRUE(*valid);
}

TEST_F(DBAppRollbackTest, ExistingAccountsRequireDerivedRecordsAndCorrectCredentials) {
  auto record = HashPassword("test-only-credential");
  ASSERT_TRUE(record);
  db().lookup_found = true;
  db().lookup_dbid = 91;
  NetworkInterface receiver(dispatcher_);
  ASSERT_TRUE(receiver.StartRudpServer(Address("127.0.0.1", 0)));
  auto connected = network_.ConnectRudpNocwnd(receiver.RudpAddress());
  ASSERT_TRUE(connected);
  std::vector<login::AuthLoginResult> responses;
  ASSERT_TRUE(receiver.InterfaceTable().RegisterTypedHandler<login::AuthLoginResult>(
      [&](const Address&, Channel*, const login::AuthLoginResult& result) {
        responses.push_back(result);
      }));
  const std::pair<std::string, std::string> cases[] = {
      {"test-only-credential", *record},
      {"incorrect", *record},
      {"test-only-credential", "test-only-credential"},
      {"test-only-credential", "$argon2id$broken"}};
  for (std::size_t i = 0; i < std::size(cases); ++i) {
    db().lookup_password_hash = cases[i].second;
    login::AuthLogin auth;
    auth.request_id = static_cast<uint32_t>(i + 1);
    auth.username = "existing_account";
    auth.password_hash = cases[i].first;
    auth_login(*connected, auth, receiver.RudpAddress());
    const auto deadline = Clock::now() + Seconds(2);
    while (responses.size() <= i && Clock::now() < deadline) dispatcher_.ProcessOnce();
    ASSERT_EQ(responses.size(), i + 1);
    EXPECT_EQ(responses.back().success, i == 0);
    EXPECT_EQ(responses.back().status,
              i == 0 ? login::LoginStatus::kSuccess : login::LoginStatus::kInvalidCredentials);
    EXPECT_EQ(responses.back().dbid, i == 0 ? DatabaseID{91} : kInvalidDBID);
  }
  EXPECT_EQ(db().put_entity_with_password_calls, 0);
  EXPECT_EQ(active_authentications(), 0u);
}

TEST_F(DBAppRollbackTest, AuthenticationBoundsWholeRequestsAndRejectsWaitersOnShutdown) {
  db().defer_name_lookups = true;
  NetworkInterface receiver(dispatcher_);
  ASSERT_TRUE(receiver.StartRudpServer(Address("127.0.0.1", 0)));
  auto connected = network_.ConnectRudpNocwnd(receiver.RudpAddress());
  ASSERT_TRUE(connected);
  std::vector<login::AuthLoginResult> responses;
  ASSERT_TRUE(receiver.InterfaceTable().RegisterTypedHandler<login::AuthLoginResult>(
      [&](const Address&, Channel*, const login::AuthLoginResult& result) {
        responses.push_back(result);
      }));
  for (uint32_t id = 1; id <= 131; ++id) {
    login::AuthLogin auth;
    auth.request_id = id;
    auth.username = "bounded_account";
    auth.password_hash = "test-only-credential";
    auth_login(*connected, auth, receiver.RudpAddress(), false);
  }
  EXPECT_EQ(active_authentications(), 2u);
  EXPECT_EQ(queued_authentications(), 128u);
  EXPECT_EQ(db().lookup_by_name_calls, 2);
  stop_authentication();
  EXPECT_EQ(queued_authentications(), 0u);
  auto callbacks = std::move(db().name_lookup_callbacks);
  ASSERT_EQ(callbacks.size(), 2u);
  for (auto& callback : callbacks) callback(LookupResult{});
  EXPECT_EQ(active_authentications(), 0u);
  login::AuthLogin after_stop;
  after_stop.request_id = 132;
  after_stop.username = "stopped_account";
  after_stop.password_hash = "test-only-credential";
  auth_login(*connected, after_stop, receiver.RudpAddress());
  dispatcher_.SetMaxPollWait(Milliseconds(1));
  const auto deadline = Clock::now() + Seconds(3);
  while (responses.size() < 132 && Clock::now() < deadline) dispatcher_.ProcessOnce();
  ASSERT_EQ(responses.size(), 132u);
  std::unordered_set<uint32_t> completed;
  for (const auto& response : responses) {
    EXPECT_TRUE(completed.insert(response.request_id).second);
    EXPECT_FALSE(response.success);
    EXPECT_EQ(response.status, response.request_id == 131 ? login::LoginStatus::kServerBusy
                                                          : login::LoginStatus::kServerNotReady);
  }
  EXPECT_EQ(db().lookup_by_name_calls, 2);
  EXPECT_EQ(db().put_entity_with_password_calls, 0);
}

TEST_F(DBAppRollbackTest, ShutdownDuringPasswordDerivationDoesNotCreateAccount) {
  set_auto_create_accounts(true, 1);
  login::AuthLogin auth;
  auth.request_id = 86;
  auth.username = "shutdown_account";
  auth.password_hash = "test-only-credential";
  auth.auto_create = true;
  auth_login(test_support::FakeChannel(0x1506), auth, Address("127.0.0.1", 30001), false);
  EXPECT_EQ(active_authentications(), 1u);
  stop_authentication();
  drain_authentication_tasks();
  EXPECT_EQ(active_authentications(), 0u);
  EXPECT_EQ(db().put_entity_with_password_calls, 0);
}

TEST_F(DBAppRollbackTest, AuthenticationCompletionAdmitsNextWaiterOnlyOnce) {
  db().defer_name_lookups = true;
  for (uint32_t id = 1; id <= 4; ++id) {
    login::AuthLogin auth;
    auth.request_id = id;
    auth.username = "queued_account";
    auth.password_hash = "test-only-credential";
    auth_login(nullptr, auth, Address("127.0.0.1", 30001), false);
  }
  EXPECT_EQ(active_authentications(), 2u);
  EXPECT_EQ(queued_authentications(), 2u);
  ASSERT_EQ(db().name_lookup_callbacks.size(), 2u);
  auto first = db().name_lookup_callbacks.front();
  first(LookupResult{});
  first(LookupResult{});
  EXPECT_EQ(active_authentications(), 2u);
  EXPECT_EQ(queued_authentications(), 1u);
  EXPECT_EQ(db().lookup_by_name_calls, 3);
  for (std::size_t i = 1; i <= 3; ++i) {
    ASSERT_GT(db().name_lookup_callbacks.size(), i);
    auto complete = db().name_lookup_callbacks[i];
    complete(LookupResult{});
  }
  EXPECT_EQ(active_authentications(), 0u);
  EXPECT_EQ(queued_authentications(), 0u);
  EXPECT_EQ(db().lookup_by_name_calls, 4);
}

TEST_F(DBAppRollbackTest, AuthLoginAutoCreateAllocatesDbidFromUsernameShard) {
  db().complete_put_immediately = false;
  set_auto_create_accounts(true, 1);
  const std::string username = "ranged_user";
  const DatabaseID route_key = DbShardRouteKey(username);
  const DatabaseID low = route_key;
  const DatabaseID high = route_key < std::numeric_limits<DatabaseID>::max() - 1
                              ? route_key + 2
                              : std::numeric_limits<DatabaseID>::max();

  dbappmgr::RegisterDbAppAck ack;
  ack.success = true;
  ack.dbapp_id = 7;
  ack.shard_table_version = 1;
  ack.entries.push_back(dbappmgr::ShardEntry{low, high, 7, Address(0x7F000001u, 24001), false});
  handle_register_dbapp_ack(ack);

  login::AuthLogin auth;
  auth.request_id = 88;
  auth.username = username;
  auth.password_hash = "pw_hash";
  auth.auto_create = true;
  auth_login(test_support::FakeChannel(0x1506), auth);

  EXPECT_EQ(db().lookup_by_name_calls, 1);
  EXPECT_EQ(db().last_lookup_identifier, username);
  EXPECT_EQ(db().put_entity_with_password_calls, 1);
  EXPECT_EQ(db().last_put_dbid, low);
  EXPECT_EQ(db().last_put_identifier, username);
  auto verified = VerifyPassword("pw_hash", db().last_password_hash);
  ASSERT_TRUE(verified);
  EXPECT_TRUE(*verified);
  EXPECT_TRUE(HasFlag(db().last_put_flags, WriteFlags::kCreateNew));
  EXPECT_TRUE(HasFlag(db().last_put_flags, WriteFlags::kExplicitDbid));
}

TEST_F(DBAppRollbackTest, AccountCreationConfirmsConflictsAndUnknownCommitsByCredentials) {
  struct Case {
    DatabaseErrorKind error_kind;
    bool found;
    std::string password_hash;
    std::string lookup_error;
    login::LoginStatus status;
  };
  auto expected = HashPassword("expected");
  auto different = HashPassword("different");
  ASSERT_TRUE(expected);
  ASSERT_TRUE(different);
  const Case cases[] = {
      {DatabaseErrorKind::kDuplicateIdentifier, true, *expected, "", login::LoginStatus::kSuccess},
      {DatabaseErrorKind::kDuplicateIdentifier, true, *different, "",
       login::LoginStatus::kInvalidCredentials},
      {DatabaseErrorKind::kDuplicateIdentifier, true, "expected", "",
       login::LoginStatus::kInvalidCredentials},
      {DatabaseErrorKind::kDuplicateIdentifier, true, "", "",
       login::LoginStatus::kInvalidCredentials},
      {DatabaseErrorKind::kCommitUnknown, true, *expected, "", login::LoginStatus::kSuccess},
      {DatabaseErrorKind::kCommitUnknown, false, "", "", login::LoginStatus::kInternalError},
      {DatabaseErrorKind::kCommitUnknown, true, *expected, "lookup failed",
       login::LoginStatus::kInternalError},
      {DatabaseErrorKind::kNone, true, *expected, "", login::LoginStatus::kInternalError},
  };
  db().complete_put_immediately = false;
  set_auto_create_accounts(true, 1);
  NetworkInterface receiver(dispatcher_);
  ASSERT_TRUE(receiver.StartRudpServer(Address("127.0.0.1", 0)));
  auto connected = network_.ConnectRudpNocwnd(receiver.RudpAddress());
  ASSERT_TRUE(connected);
  std::vector<login::AuthLoginResult> responses;
  ASSERT_TRUE(receiver.InterfaceTable().RegisterTypedHandler<login::AuthLoginResult>(
      [&](const Address&, Channel*, const login::AuthLoginResult& result) {
        responses.push_back(result);
      }));
  dispatcher_.SetMaxPollWait(Milliseconds(1));
  uint32_t request_id = 2000;
  for (const auto& entry : cases) {
    SCOPED_TRACE(request_id);
    db().lookup_found = false;
    db().lookup_error.clear();
    login::AuthLogin auth;
    auth.request_id = request_id++;
    auth.username = "racing_account";
    auth.password_hash = "expected";
    auth.auto_create = true;
    auth_login(*connected, auth, receiver.RudpAddress());
    ASSERT_TRUE(db().put_callback);
    db().lookup_found = entry.found;
    db().lookup_dbid = 99;
    db().lookup_password_hash = entry.password_hash;
    db().lookup_error = entry.lookup_error;
    const auto lookups_before = db().lookup_by_name_calls;
    auto complete = std::move(db().put_callback);
    const auto responses_before = responses.size();
    complete(PutResult{false, 42, "injected write failure", entry.error_kind});
    const auto deadline = Clock::now() + Seconds(2);
    while (responses.size() == responses_before && Clock::now() < deadline) {
      dispatcher_.ProcessOnce();
    }
    ASSERT_EQ(responses.size(), responses_before + 1);
    const auto& result = responses.back();
    EXPECT_EQ(result.request_id, auth.request_id);
    EXPECT_EQ(result.status, entry.status);
    EXPECT_EQ(result.success, entry.status == login::LoginStatus::kSuccess);
    EXPECT_EQ(result.dbid, result.success ? DatabaseID{99} : kInvalidDBID);
    EXPECT_EQ(db().lookup_by_name_calls - lookups_before,
              entry.error_kind == DatabaseErrorKind::kNone ? 0 : 1);
  }
  EXPECT_EQ(db().put_entity_with_password_calls, std::size(cases));
}

TEST_F(DBAppRollbackTest, AuthLoginAutoCreateRetriesDuplicateDbidInsideUsernameShard) {
  db().duplicate_dbid_failures_before_success = 1;
  set_auto_create_accounts(true, 1);
  const std::string username = "duplicate_retry_user";
  const DatabaseID route_key = DbShardRouteKey(username);
  const DatabaseID low = route_key;
  const DatabaseID high = route_key < std::numeric_limits<DatabaseID>::max() - 2
                              ? route_key + 3
                              : std::numeric_limits<DatabaseID>::max();

  dbappmgr::RegisterDbAppAck ack;
  ack.success = true;
  ack.dbapp_id = 7;
  ack.shard_table_version = 1;
  ack.entries.push_back(dbappmgr::ShardEntry{low, high, 7, Address(0x7F000001u, 24001), false});
  handle_register_dbapp_ack(ack);

  login::AuthLogin auth;
  auth.request_id = 89;
  auth.username = username;
  auth.password_hash = "pw_hash";
  auth.auto_create = true;
  auth_login(test_support::FakeChannel(0x1506), auth);

  EXPECT_EQ(db().put_entity_with_password_calls, 2);
  EXPECT_EQ(db().last_put_dbid, low + 1);
  EXPECT_EQ(db().last_put_identifier, username);
  auto verified = VerifyPassword("pw_hash", db().last_password_hash);
  ASSERT_TRUE(verified);
  EXPECT_TRUE(*verified);
  EXPECT_TRUE(HasFlag(db().last_put_flags, WriteFlags::kCreateNew));
  EXPECT_TRUE(HasFlag(db().last_put_flags, WriteFlags::kExplicitDbid));
}

TEST_F(DBAppRollbackTest, WriteEntityRejectsStaleShardTableVersion) {
  set_shard_table_version(9);
  InterfaceTable channel_table;
  CapturingChannel reply(dispatcher_, channel_table, Address(0x7F000001u, 30001));

  dbapp::WriteEntity msg;
  msg.blob = DatabaseBlob();
  msg.owner.process_instance = 111;
  msg.operation_id[0] = std::byte{1};
  msg.request_id = 44;
  msg.dbid = 777;
  msg.type_id = 9;
  msg.shard_table_version = 8;
  write_entity(&reply, msg);

  const auto acks = DecodeSentMessages<dbapp::WriteEntityAck>(reply);
  ASSERT_EQ(acks.size(), 1u);
  EXPECT_EQ(acks[0].request_id, 44u);
  EXPECT_FALSE(acks[0].success);
  EXPECT_EQ(acks[0].dbid, 777);
  EXPECT_EQ(acks[0].current_shard_table_version, 9u);
  EXPECT_EQ(acks[0].error, std::string(dbapp::kInvalidShardTableError));
}

TEST_F(DBAppRollbackTest, CheckoutEntityRejectsStaleShardTableVersion) {
  set_shard_table_version(9);
  InterfaceTable channel_table;
  CapturingChannel reply(dispatcher_, channel_table, Address(0x7F000001u, 30001));

  dbapp::CheckoutEntity msg;
  msg.owner_process_instance = 111;
  msg.operation_id[0] = std::byte{1};
  msg.request_id = 45;
  msg.dbid = 778;
  msg.type_id = 9;
  msg.shard_table_version = 8;
  checkout_entity(&reply, msg);

  const auto acks = DecodeSentMessages<dbapp::CheckoutEntityAck>(reply);
  ASSERT_EQ(acks.size(), 1u);
  EXPECT_EQ(acks[0].request_id, 45u);
  EXPECT_EQ(acks[0].status, dbapp::CheckoutStatus::kDbError);
  EXPECT_EQ(acks[0].dbid, 778);
  EXPECT_EQ(acks[0].current_shard_table_version, 9u);
  EXPECT_EQ(acks[0].error, std::string(dbapp::kInvalidShardTableError));
  EXPECT_FALSE(pending_request_contains(45));
  EXPECT_FALSE(checkout_owner(778, 9).has_value());
}

TEST_F(DBAppRollbackTest, ProcessDeathRetainsMemoryUntilRetirementIsConfirmed) {
  const Address address("127.0.0.1", 30001);
  CheckoutInfo owner{address, 7, 9, 42, 111};
  replace_checkout_owner(555, 9, owner);
  retire_process(address, 111);
  EXPECT_EQ(db().retire_calls, 1);
  EXPECT_EQ(db().retired_instance, 111u);
  EXPECT_EQ(retirement_count(), 1u);
  EXPECT_TRUE(checkout_owner(555, 9));
  complete_retirement(Error{ErrorCode::kCommitUnknown, "injected commit uncertainty"});
  EXPECT_TRUE(checkout_owner(555, 9));
  EXPECT_EQ(retirement_count(), 1u);
  retry_retirement(address, 111);
  EXPECT_EQ(db().retire_calls, 2);
  complete_retirement(1);
  EXPECT_FALSE(checkout_owner(555, 9));
  EXPECT_EQ(retirement_count(), 0u);
}

TEST_F(DBAppRollbackTest, OldProcessDeathCompletionPreservesNewAddressOwner) {
  const Address address("127.0.0.1", 30001);
  replace_checkout_owner(555, 9, CheckoutInfo{address, 7, 9, 42, 111});
  retire_process(address, 111);
  replace_checkout_owner(555, 9, CheckoutInfo{address, 7, 9, 43, 222});
  complete_retirement(1);
  ASSERT_TRUE(checkout_owner(555, 9));
  EXPECT_EQ(checkout_owner(555, 9)->process_instance, 222u);
  retire_process(address, 111);
  complete_retirement(0);
  ASSERT_TRUE(checkout_owner(555, 9));
  EXPECT_EQ(checkout_owner(555, 9)->process_instance, 222u);
}

TEST_F(DBAppRollbackTest, DeadProcessPendingCallbackCannotPublishCheckout) {
  const Address address("127.0.0.1", 30001);
  start_checkout(1001, 555, 9);
  ASSERT_TRUE(db().checkout_callback);
  retire_process(address, 111);
  complete_retirement(0);
  complete_checkout_success(555, 42);
  EXPECT_FALSE(checkout_owner(555, 9));
  EXPECT_FALSE(pending_request_contains(1001));
  EXPECT_EQ(db().release_calls, 1);
  EXPECT_EQ(db().last_release_owner.process_instance, 111u);
}

TEST_F(DBAppRollbackTest, MissingProcessIdentityCannotAcquireOrTriggerDeathCleanup) {
  const Address address("127.0.0.1", 30001);
  retire_process(address, 0);
  EXPECT_EQ(db().retire_calls, 0);
  InterfaceTable table;
  CapturingChannel reply(dispatcher_, table, address);
  dbapp::CheckoutEntity request;
  request.operation_id[0] = std::byte{1};
  request.dbid = 555;
  request.type_id = 9;
  checkout_from(address, &reply, request);
  EXPECT_FALSE(db().checkout_callback);
  const auto acks = DecodeSentMessages<dbapp::CheckoutEntityAck>(reply);
  ASSERT_EQ(acks.size(), 1u);
  EXPECT_EQ(acks[0].status, dbapp::CheckoutStatus::kDbError);
}

TEST_F(BaseAppRollbackTest, CheckinRetainsLeaseAcrossFailedSendAndMatchesCompleteReply) {
  const Address target("127.0.0.1", 24001);
  const CheckoutInfo owner{Address("127.0.0.1", 25001), 7, 9, 11, 111};
  release_checkout(42, owner);
  EXPECT_EQ(pending_checkins(), 1u);
  EXPECT_TRUE(active_login_contains(42));
  InterfaceTable table;
  CapturingChannel channel(dispatcher_, table, target);
  set_legacy_dbapp_channel(&channel);
  retry_checkin_now(1);
  auto requests = DecodeSentMessages<dbapp::CheckinEntity>(channel);
  ASSERT_EQ(requests.size(), 1u);
  const auto request = requests[0];
  release_checkout(42, owner);
  EXPECT_EQ(pending_checkins(), 1u);
  checkin_ack(Address("127.0.0.1", 24002), {request, true, {}});
  auto wrong = request;
  ++wrong.owner.process_instance;
  checkin_ack(target, {wrong, true, {}});
  wrong = request;
  ++wrong.dbid;
  checkin_ack(target, {wrong, true, {}});
  checkin_ack(target, {request, false, "database unavailable"});
  EXPECT_EQ(pending_checkins(), 1u);
  retry_checkin_now(request.request_id);
  requests = DecodeSentMessages<dbapp::CheckinEntity>(channel);
  ASSERT_EQ(requests.size(), 2u);
  EXPECT_EQ(requests[1].request_id, request.request_id);
  EXPECT_EQ(requests[1].owner, owner);
  retry_checkin_now(request.request_id);
  EXPECT_EQ(pending_checkins(), 1u);
  EXPECT_TRUE(active_login_contains(42));
  EXPECT_EQ(DecodeSentMessages<dbapp::CheckinEntity>(channel).size(), 2u);
  checkin_ack(target, {request, true, {}});
  EXPECT_EQ(pending_checkins(), 0u);
  EXPECT_FALSE(active_login_contains(42));
  checkin_ack(target, {request, true, {}});
  EXPECT_EQ(pending_checkins(), 0u);
}

TEST_F(DBAppRollbackTest, CheckinReadbackCannotClearCurrentOrNewerLocalOwner) {
  start_checkout(1001, 555, 9);
  complete_checkout_success(555, 42);
  dbapp::CheckinEntity msg;
  msg.dbid = 555;
  msg.type_id = 9;
  msg.owner = *checkout_owner(555, 9);
  db().release_success = false;
  db().readback.success = true;
  db().readback.checked_out_by = msg.owner;
  checkin_entity(msg);
  EXPECT_TRUE(checkout_owner(555, 9));
  auto newer = msg.owner;
  ++newer.ownership_version;
  replace_checkout_owner(555, 9, newer);
  db().readback.checked_out_by = newer;
  checkin_entity(msg);
  ASSERT_TRUE(checkout_owner(555, 9));
  EXPECT_EQ(checkout_owner(555, 9)->ownership_version, newer.ownership_version);
  EXPECT_EQ(db().get_entity_calls, 2);
}

}  // namespace atlas
