#include <array>
#include <format>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "baseapp/baseapp.h"
#include "coro/cancellation.h"
#include "db/idatabase.h"
#include "dbapp/dbapp.h"
#include "dbappmgr/dbappmgr_messages.h"
#include "loginapp/loginapp.h"
#include "network/reliable_udp.h"
#include "serialization/binary_stream.h"
#include "server/db_shard_routing.h"
#include "test_null_channel.h"

namespace atlas {

namespace {

class CapturingChannel final : public Channel {
 public:
  using Channel::Channel;

  [[nodiscard]] auto Fd() const -> FdHandle override { return kInvalidFd; }
  [[nodiscard]] auto send_count() const -> std::size_t { return sends_.size(); }
  [[nodiscard]] auto Sends() const -> const std::vector<std::vector<std::byte>>& { return sends_; }

 protected:
  [[nodiscard]] auto DoSend(std::span<const std::byte> data) -> Result<std::size_t> override {
    sends_.emplace_back(data.begin(), data.end());
    return data.size();
  }

 private:
  std::vector<std::vector<std::byte>> sends_;
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
  void handle_auth_result(const login::AuthLoginResult& result) {
    BinaryWriter writer;
    result.Serialize(writer);
    ASSERT_TRUE(
        app_.rpc_registry_.TryDispatch(login::AuthLoginResult::Descriptor().id, writer.Data()));
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
  handle_auth_result(auth_reply);

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
  EXPECT_EQ(retried_writes[0].blob, std::vector<std::byte>(blob.begin(), blob.end()));

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
  reply.request_id = checkout.request_id;
  reply.operation_id = checkout.operation_id;
  reply.dbid = 42;
  reply.checkout_version = 11;
  reply.holder_app_id = checkout.owner_app_id;
  reply.holder_process_instance = 111;
  reply.holder_entity_id = checkout.entity_id;
  reply.holder_process_instance = 111;
  reply.blob = {std::byte{0xA7}, std::byte{1}, std::byte{1},    std::byte{0},
                std::byte{0},    std::byte{0}, std::byte{0x11}, std::byte{1},
                std::byte{0},    std::byte{0}, std::byte{0},    std::byte{0x22}};
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
                     std::function<void(bool)> callback) override {
    ++clear_checkout_calls;
    last_cleared = std::make_pair(dbid, type_id);
    callback(true);
  }

  void ClearCheckoutsForAddress(const Address&, std::function<void(int)> callback) override {
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

  void MarkCheckoutCleared(DatabaseID dbid, uint16_t type_id) override {
    ++mark_checkout_cleared_calls;
    last_cleared = std::make_pair(dbid, type_id);
  }

  void GetAutoLoadEntities(std::function<void(std::vector<EntityData>)> callback) override {
    callback({});
  }

  void SetAutoLoad(DatabaseID, uint16_t, bool) override {}
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
    app_.database_ = std::make_unique<FakeDatabase>();
  }

  auto db() -> FakeDatabase& { return *static_cast<FakeDatabase*>(app_.database_.get()); }

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
                  const Address& src = Address("127.0.0.1", 30001)) {
    app_.OnAuthLogin(src, ch, auth);
  }

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

TEST_F(DBAppRollbackTest, SharedWriteRejectsOwnerFromDifferentChannel) {
  db().shared = true;
  InterfaceTable table;
  CapturingChannel reply(dispatcher_, table, Address(0x7F000001, 30001));
  dbapp::WriteEntity write;
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
  write.owner.process_instance = 111;
  write.operation_id[0] = std::byte{1};
  write.request_id = 1001;
  write.dbid = 42;
  write.type_id = 9;
  write.blob = {std::byte{1}};
  write_entity(&reply, write);
  ASSERT_TRUE(write_pending(write));
  auto changed = write;
  changed.blob[0] = std::byte{2};
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

TEST_F(DBAppRollbackTest, SingleDbappLogoffForwardsLeaseAndKeepsNewOwnerOnLateCompletion) {
  start_checkout(1001, 555, 9);
  complete_checkout_success(555, 42);
  db().complete_put_immediately = false;
  dbapp::WriteEntity write;
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
  EXPECT_EQ(db().last_password_hash, "pw_hash");
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
  const Case cases[] = {
      {DatabaseErrorKind::kDuplicateIdentifier, true, "expected", "", login::LoginStatus::kSuccess},
      {DatabaseErrorKind::kDuplicateIdentifier, true, "different", "",
       login::LoginStatus::kInvalidCredentials},
      {DatabaseErrorKind::kDuplicateIdentifier, true, "", "",
       login::LoginStatus::kInvalidCredentials},
      {DatabaseErrorKind::kCommitUnknown, true, "expected", "", login::LoginStatus::kSuccess},
      {DatabaseErrorKind::kCommitUnknown, false, "", "", login::LoginStatus::kInternalError},
      {DatabaseErrorKind::kCommitUnknown, true, "expected", "lookup failed",
       login::LoginStatus::kInternalError},
      {DatabaseErrorKind::kNone, true, "expected", "", login::LoginStatus::kInternalError},
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
  EXPECT_EQ(db().last_password_hash, "pw_hash");
  EXPECT_TRUE(HasFlag(db().last_put_flags, WriteFlags::kCreateNew));
  EXPECT_TRUE(HasFlag(db().last_put_flags, WriteFlags::kExplicitDbid));
}

TEST_F(DBAppRollbackTest, WriteEntityRejectsStaleShardTableVersion) {
  set_shard_table_version(9);
  InterfaceTable channel_table;
  CapturingChannel reply(dispatcher_, channel_table, Address(0x7F000001u, 30001));

  dbapp::WriteEntity msg;
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
