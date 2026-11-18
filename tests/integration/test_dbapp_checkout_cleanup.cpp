#define _CRT_SECURE_NO_WARNINGS

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <future>
#include <optional>
#include <string_view>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "atdf_account_fixture.h"
#include "db/entity_blob.h"
#include "dbapp/account_session_messages.h"
#include "dbapp/dbapp.h"
#include "dbapp/dbapp_messages.h"
#include "mysql_test_fixture.h"
#include "network/event_dispatcher.h"
#include "network/machined_types.h"
#include "network/network_interface.h"
#include "network/reliable_udp.h"
#include "network/socket.h"
#include "platform/child_process.h"
#include "platform/filesystem.h"

using namespace atlas;
using namespace atlas::dbapp;

namespace {

template <typename Pred>
bool poll_until(EventDispatcher& disp, Pred pred,
                std::chrono::milliseconds timeout = std::chrono::milliseconds(3000)) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    disp.ProcessOnce();
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return false;
}

auto reserve_udp_port() -> uint16_t {
  auto sock = Socket::CreateUdp();
  EXPECT_TRUE(sock.HasValue());
  EXPECT_TRUE(sock->Bind(Address("127.0.0.1", 0)).HasValue());
  auto local = sock->LocalAddress();
  EXPECT_TRUE(local.HasValue());
  return local ? local->Port() : 0;
}

auto write_entity_defs_bin() -> std::filesystem::path {
  auto path = std::filesystem::temp_directory_path() / "atlas_dbapp_checkout_entity_defs.bin";
  return atlas::test_fixtures::WriteAccountAtdfFile(path);
}

struct DBAppArgv {
  DBAppArgv(uint16_t internal_port, const std::filesystem::path& entitydef_bin_path)
      : storage{"dbapp",
                "--type",
                "dbapp",
                "--name",
                "dbapp_test",
                "--update-hertz",
                "100",
                "--internal-port",
                std::to_string(internal_port),
                "--machined",
                "127.0.0.1:1",
                "--entitydef-bin-path",
                entitydef_bin_path.string(),
                "--account-type-id",
                "1",
                "--auto-create-accounts",
                "true"} {
    auto database = test_fixtures::MysqlTestArguments();
    storage.insert(storage.end(), database.begin(), database.end());
    for (auto& value : storage) ptrs.push_back(value.data());
  }

  int argc() { return static_cast<int>(ptrs.size()); }
  char** argv() { return ptrs.data(); }

  std::vector<std::string> storage;
  std::vector<char*> ptrs;
};

class TestDBApp final : public DBApp {
 public:
  TestDBApp(EventDispatcher& dispatcher, NetworkInterface& network, std::promise<Address>& addr,
            std::atomic<bool>& stop_flag)
      : DBApp(dispatcher, network), addr_promise_(addr), stop_flag_(stop_flag) {}

 protected:
  auto Init(int argc, char* argv[]) -> bool override {
    if (!DBApp::Init(argc, argv)) {
      addr_promise_.set_value(Address{});
      return false;
    }

    Address addr = Network().RudpAddress();
    if (addr.Ip() == 0) addr = Address("127.0.0.1", addr.Port());
    addr_promise_.set_value(addr);
    return true;
  }

  void OnTickComplete() override {
    DBApp::OnTickComplete();
    if (stop_flag_.load(std::memory_order_acquire)) Shutdown();
  }

 private:
  std::promise<Address>& addr_promise_;
  std::atomic<bool>& stop_flag_;
};

struct DBAppClient {
  explicit DBAppClient(std::string name) : dispatcher(std::move(name)), network(dispatcher) {
    dispatcher.SetMaxPollWait(Milliseconds(1));
    (void)network.InterfaceTable().RegisterTypedHandler<AccountSessionReply>(
        [this](const Address&, Channel*, const AccountSessionReply& message) {
          session_reply = message;
        });
    (void)network.InterfaceTable().RegisterTypedHandler<WriteEntityAck>(
        [this](const Address&, Channel*, const WriteEntityAck& msg) {
          write_ack = msg;
          write_received.store(true, std::memory_order_release);
        });
    (void)network.InterfaceTable().RegisterTypedHandler<DeleteEntityAck>(
        [this](const Address&, Channel*, const DeleteEntityAck& msg) {
          delete_ack = msg;
          delete_received.store(true, std::memory_order_release);
        });
    (void)network.InterfaceTable().RegisterTypedHandler<CheckinEntityAck>(
        [this](const Address&, Channel*, const CheckinEntityAck& msg) {
          checkin_ack = msg;
          if (drop_checkin_reply) {
            ++dropped_checkin_count;
            return;
          }
          checkin_received.store(true, std::memory_order_release);
        });
    (void)network.InterfaceTable().RegisterTypedHandler<AbortCheckoutAck>(
        [this](const Address&, Channel*, const AbortCheckoutAck& msg) {
          abort_ack = msg;
          abort_received.store(true, std::memory_order_release);
        });
    (void)network.InterfaceTable().RegisterTypedHandler<CheckoutEntityAck>(
        [this](const Address&, Channel*, const CheckoutEntityAck& msg) {
          checkout_ack = msg;
          if (drop_checkout_reply) {
            ++dropped_checkout_count;
            return;
          }
          checkout_received.store(true, std::memory_order_release);
        });
  }

  void start_local_rudp(uint16_t port) {
    ASSERT_TRUE(network.StartRudpServer(Address("127.0.0.1", port)).HasValue());
  }

  [[nodiscard]] auto local_addr() const -> Address { return network.RudpAddress(); }
  void reset_write() { write_received.store(false, std::memory_order_release); }
  void reset_checkout() { checkout_received.store(false, std::memory_order_release); }

  EventDispatcher dispatcher;
  NetworkInterface network;
  std::atomic<bool> write_received{false};
  std::atomic<bool> checkout_received{false};
  DeleteEntityAck delete_ack;
  CheckinEntityAck checkin_ack;
  std::atomic<bool> checkin_received{false};
  bool drop_checkin_reply{false};
  uint32_t dropped_checkin_count{0};
  std::atomic<bool> delete_received{false};
  AbortCheckoutAck abort_ack;
  std::atomic<bool> abort_received{false};
  WriteEntityAck write_ack;
  CheckoutEntityAck checkout_ack;
  std::optional<AccountSessionReply> session_reply;
  bool drop_checkout_reply{false};
  uint32_t dropped_checkout_count{0};
};

auto make_blob(std::string_view text) -> std::vector<std::byte> {
  std::vector<std::byte> blob;
  for (char ch : text) blob.push_back(static_cast<std::byte>(ch));
  return blob;
}

void CheckAccountSessionProtocol(DBAppClient& first, Channel& first_channel, DBAppClient& next,
                                 Channel& next_channel, DatabaseID dbid,
                                 const CheckoutInfo& checkout) {
  uint64_t request_id = uint64_t{1} << 40;
  auto exchange = [&](DBAppClient& client, Channel& channel,
                      std::optional<AccountSessionOperation> operation) -> Result<AccountSession> {
    client.session_reply.reset();
    AccountSessionRequest request;
    request.request_id = ++request_id;
    request.process_instance = 111;
    request.account_dbid = dbid;
    request.account_type_id = 1;
    request.operation = std::move(operation);
    if (auto sent = channel.SendMessage(request); !sent) return sent.Error();
    if (!poll_until(client.dispatcher, [&] { return client.session_reply.has_value(); }))
      return Error{ErrorCode::kTimeout, "account session reply missing"};
    const auto& reply = *client.session_reply;
    EXPECT_EQ(reply.request_id, request.request_id);
    EXPECT_EQ(reply.requester_instance, request.process_instance);
    EXPECT_EQ(reply.account_dbid, dbid);
    EXPECT_EQ(reply.account_type_id, 1);
    if (request.operation) {
      EXPECT_EQ(reply.operation_id, request.operation->id);
    }
    if (reply.status != ErrorCode::kNone) return Error{reply.status, reply.error};
    return reply.session;
  };
  auto initial = exchange(first, first_channel, {});
  ASSERT_TRUE(initial.HasValue()) << initial.Error().Message();
  EXPECT_EQ(initial->phase, AccountSessionPhase::kOffline);
  AccountSessionOperation operation;
  ASSERT_TRUE(FillSecureRandom(operation.id));
  operation.account_dbid = dbid;
  operation.account_type_id = 1;
  operation.expected_version = initial->version;
  operation.actor = {checkout.base_addr, checkout.entity_id, checkout.process_instance};
  operation.claim_checkout = checkout;
  auto active = exchange(first, first_channel, operation);
  ASSERT_TRUE(active.HasValue()) << active.Error().Message();
  const auto source = operation.actor;
  operation.claim_checkout.reset();
  operation.expected_version = active->version;
  operation.action = AccountSessionAction::kPrepare;
  operation.target = {next.local_addr(), 2002, 111};
  ASSERT_TRUE(FillSecureRandom(operation.id));
  ASSERT_TRUE(FillSecureRandom(operation.handoff_id));
  ASSERT_TRUE(FillSecureRandom(operation.credential_hash));
  auto preparing = exchange(first, first_channel, operation);
  ASSERT_TRUE(preparing.HasValue()) << preparing.Error().Message();
  operation.actor = operation.target;
  operation.expected_version = preparing->version;
  operation.action = AccountSessionAction::kAccept;
  ASSERT_TRUE(FillSecureRandom(operation.id));
  auto accepted = exchange(next, next_channel, operation);
  ASSERT_TRUE(accepted.HasValue()) << accepted.Error().Message();
  operation.actor = source;
  operation.expected_version = accepted->version;
  operation.action = AccountSessionAction::kAwaitClient;
  ASSERT_TRUE(FillSecureRandom(operation.id));
  auto awaiting = exchange(first, first_channel, operation);
  ASSERT_TRUE(awaiting.HasValue()) << awaiting.Error().Message();
  operation.actor = operation.target;
  operation.expected_version = awaiting->version;
  operation.action = AccountSessionAction::kComplete;
  ASSERT_TRUE(FillSecureRandom(operation.id));
  auto transferred = exchange(next, next_channel, operation);
  ASSERT_TRUE(transferred.HasValue()) << transferred.Error().Message();
  EXPECT_EQ(transferred->owner, operation.actor);
  auto repeated = exchange(next, next_channel, operation);
  ASSERT_TRUE(repeated.HasValue()) << repeated.Error().Message();
  EXPECT_EQ(*repeated, *transferred);
  operation.expected_version = transferred->version;
  operation.action = AccountSessionAction::kRelease;
  ASSERT_TRUE(FillSecureRandom(operation.id));
  auto offline = exchange(next, next_channel, operation);
  ASSERT_TRUE(offline.HasValue()) << offline.Error().Message();
  EXPECT_EQ(offline->phase, AccountSessionPhase::kOffline);
}

}  // namespace

TEST(DBAppIntegration, InvalidDefinitionsCompleteTheStartupNotification) {
  EventDispatcher dispatcher{"invalid_dbapp_definitions"};
  NetworkInterface network(dispatcher);
  std::promise<Address> ready;
  auto address = ready.get_future();
  std::atomic<bool> stop{false};
  TestDBApp app(dispatcher, network, ready, stop);
  const auto stamp = Clock::now().time_since_epoch().count();
  const auto directory =
      std::filesystem::temp_directory_path() / std::format("atlas_invalid_dbapp_{}", stamp);
  DBAppArgv arguments(reserve_udp_port(), directory / "missing.bin");
  EXPECT_EQ(app.RunApp(arguments.argc(), arguments.argv()), 1);
  ASSERT_EQ(address.wait_for(std::chrono::seconds(0)), std::future_status::ready);
  EXPECT_EQ(address.get(), Address{});
}

TEST(DBAppIntegration, BaseAppDeathNotificationClearsCheckoutOwnership) {
  const uint16_t port = reserve_udp_port();
  ASSERT_NE(port, 0u);

  const auto entity_defs = write_entity_defs_bin();
  ASSERT_TRUE(test_fixtures::ResetMysqlTestDatabase());

  std::promise<Address> addr_promise;
  auto addr_future = addr_promise.get_future();
  std::atomic<bool> stop_flag{false};
  DBAppClient machined_client{"dbapp_fake_machined"};
  ASSERT_TRUE(machined_client.network.StartTcpServer(Address("127.0.0.1", 0)));
  Channel* machined_channel = nullptr;
  ASSERT_TRUE(
      machined_client.network.InterfaceTable().RegisterTypedHandler<machined::RegisterMessage>(
          [&](const Address&, Channel* channel, const machined::RegisterMessage&) {
            machined_channel = channel;
            (void)channel->SendMessage(machined::RegisterAck{});
          }));

  std::thread app_thread([&]() {
    EventDispatcher dispatcher{"dbapp_server"};
    dispatcher.SetMaxPollWait(Milliseconds(1));
    NetworkInterface network(dispatcher);
    TestDBApp app(dispatcher, network, addr_promise, stop_flag);
    DBAppArgv args(port, entity_defs);
    for (std::size_t i = 0; i + 1 < args.storage.size(); ++i) {
      if (args.storage[i] == "--machined") {
        args.storage[i + 1] = machined_client.network.TcpAddress().ToString();
        args.ptrs[i + 1] = args.storage[i + 1].data();
      }
    }
    EXPECT_EQ(app.RunApp(args.argc(), args.argv()), 0);
  });

  const Address server_addr = addr_future.get();
  ASSERT_NE(server_addr.Port(), 0u);

  DBAppClient baseapp1{"dbapp_baseapp1"};
  DBAppClient baseapp2{"dbapp_baseapp2"};
  baseapp1.start_local_rudp(reserve_udp_port());
  baseapp2.start_local_rudp(reserve_udp_port());

  auto ch1 = baseapp1.network.ConnectRudp(server_addr);
  auto ch2 = baseapp2.network.ConnectRudp(server_addr);
  ASSERT_TRUE(ch1.HasValue()) << ch1.Error().Message();
  ASSERT_TRUE(ch2.HasValue()) << ch2.Error().Message();
  ASSERT_TRUE(poll_until(machined_client.dispatcher, [&] { return machined_channel != nullptr; }));

  WriteEntity write;
  write.entity_def_digest = atlas::test_fixtures::kAccountDefinitionDigest;
  write.owner.process_instance = 111;
  write.operation_id[0] = std::byte{1};
  write.flags = WriteFlags::kCreateNew;
  write.type_id = 1;
  write.entity_id = 101;
  write.request_id = 1;
  write.identifier = "cleanup_target";
  auto encoded = EncodeDbBlob(make_blob("entity"), {});
  ASSERT_TRUE(encoded);
  write.blob = std::move(*encoded);
  ASSERT_TRUE((*ch1)->SendMessage(write).HasValue());
  ASSERT_TRUE(poll_until(baseapp1.dispatcher,
                         [&] { return baseapp1.write_received.load(std::memory_order_acquire); }));
  ASSERT_TRUE(baseapp1.write_ack.success);
  const auto dbid = baseapp1.write_ack.dbid;
  ASSERT_GT(dbid, 0);

  baseapp1.reset_write();
  ASSERT_TRUE((*ch1)->SendMessage(write).HasValue());
  ASSERT_TRUE(poll_until(baseapp1.dispatcher,
                         [&] { return baseapp1.write_received.load(std::memory_order_acquire); }));
  EXPECT_TRUE(baseapp1.write_ack.success);
  EXPECT_EQ(baseapp1.write_ack.dbid, dbid);

  ASSERT_GT(baseapp1.write_ack.owner.ownership_version, 0u);
  WriteEntity initial_logoff = write;
  initial_logoff.operation_id[0] = std::byte{3};
  initial_logoff.request_id = 10;
  initial_logoff.dbid = dbid;
  initial_logoff.owner = baseapp1.write_ack.owner;
  initial_logoff.flags = WriteFlags::kLogOff;
  baseapp1.reset_write();
  ASSERT_TRUE((*ch1)->SendMessage(initial_logoff).HasValue());
  ASSERT_TRUE(poll_until(baseapp1.dispatcher,
                         [&] { return baseapp1.write_received.load(std::memory_order_acquire); }));
  ASSERT_TRUE(baseapp1.write_ack.success) << baseapp1.write_ack.error;

  CheckoutEntity co1;
  co1.entity_def_digest = atlas::test_fixtures::kAccountDefinitionDigest;
  co1.owner_process_instance = 111;
  ASSERT_TRUE(FillSecureRandom(co1.operation_id));
  co1.mode = LoadMode::kByDbid;
  co1.type_id = 1;
  co1.dbid = dbid;
  co1.entity_id = 1001;
  co1.request_id = 11;
  co1.owner_addr = baseapp1.local_addr();
  auto fault_db = test_fixtures::MysqlTestConnection();
  ASSERT_NE(fault_db, nullptr);
  ASSERT_EQ(mysql_query(fault_db.get(),
                        "CREATE TRIGGER reject_checkout BEFORE UPDATE ON entities FOR EACH ROW "
                        "SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='injected checkout failure'"),
            0);
  ASSERT_TRUE((*ch1)->SendMessage(co1).HasValue());
  ASSERT_TRUE(poll_until(baseapp1.dispatcher, [&] {
    return baseapp1.checkout_received.load(std::memory_order_acquire);
  }));
  EXPECT_EQ(baseapp1.checkout_ack.status, CheckoutStatus::kDbError);
  ASSERT_EQ(mysql_query(fault_db.get(), "DROP TRIGGER reject_checkout"), 0);
  co1.request_id = 13;
  baseapp1.reset_checkout();
  ASSERT_TRUE((*ch1)->SendMessage(co1).HasValue());
  ASSERT_TRUE(poll_until(baseapp1.dispatcher, [&] {
    return baseapp1.checkout_received.load(std::memory_order_acquire);
  }));
  ASSERT_EQ(baseapp1.checkout_ack.status, CheckoutStatus::kSuccess);
  EXPECT_EQ(baseapp1.checkout_ack.dbid, dbid);

  baseapp1.reset_checkout();
  ASSERT_TRUE((*ch1)->SendMessage(co1).HasValue());
  ASSERT_TRUE(poll_until(baseapp1.dispatcher, [&] {
    return baseapp1.checkout_received.load(std::memory_order_acquire);
  }));
  EXPECT_EQ(baseapp1.checkout_ack.status, CheckoutStatus::kSuccess);
  EXPECT_EQ(baseapp1.checkout_ack.dbid, dbid);

  ASSERT_EQ(mysql_query(fault_db.get(),
                        "CREATE TRIGGER reject_logoff BEFORE UPDATE ON entities FOR EACH ROW "
                        "SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='injected logoff failure'"),
            0);
  WriteEntity logoff = write;
  logoff.operation_id[0] = std::byte{2};
  logoff.flags = WriteFlags::kLogOff;
  logoff.dbid = dbid;
  logoff.owner = CheckoutInfo{
      baseapp1.checkout_ack.holder_addr, baseapp1.checkout_ack.holder_app_id,
      baseapp1.checkout_ack.holder_entity_id, baseapp1.checkout_ack.checkout_version, 111};
  logoff.request_id = 2;
  baseapp1.reset_write();
  ASSERT_TRUE((*ch1)->SendMessage(logoff).HasValue());
  ASSERT_TRUE(poll_until(baseapp1.dispatcher,
                         [&] { return baseapp1.write_received.load(std::memory_order_acquire); }));
  EXPECT_FALSE(baseapp1.write_ack.success);
  CheckoutEntity conflict = co1;
  ASSERT_TRUE(FillSecureRandom(conflict.operation_id));
  conflict.request_id = 14;
  conflict.entity_id = 2002;
  conflict.owner_addr = baseapp2.local_addr();
  ASSERT_TRUE((*ch2)->SendMessage(conflict).HasValue());
  ASSERT_TRUE(poll_until(baseapp2.dispatcher, [&] {
    return baseapp2.checkout_received.load(std::memory_order_acquire);
  }));
  EXPECT_EQ(baseapp2.checkout_ack.status, CheckoutStatus::kAlreadyCheckedOut);
  EXPECT_EQ(baseapp2.checkout_ack.holder_addr, baseapp1.local_addr());
  EXPECT_EQ(baseapp2.checkout_ack.holder_entity_id, co1.entity_id);
  ASSERT_EQ(mysql_query(fault_db.get(), "DROP TRIGGER reject_logoff"), 0);
  fault_db.reset();
  baseapp2.reset_checkout();

  machined::DeathNotification death;
  death.process_type = ProcessType::kBaseApp;
  death.name = "dead_baseapp";
  death.internal_addr = baseapp1.local_addr();
  death.reason = 1;
  death.process_instance = co1.owner_process_instance;
  ASSERT_TRUE((*ch2)->SendMessage(death));
  ASSERT_TRUE(FillSecureRandom(conflict.operation_id));
  conflict.request_id = 15;
  ASSERT_TRUE((*ch2)->SendMessage(conflict));
  ASSERT_TRUE(poll_until(baseapp2.dispatcher, [&] {
    return baseapp2.checkout_received.load(std::memory_order_acquire);
  }));
  EXPECT_EQ(baseapp2.checkout_ack.status, CheckoutStatus::kAlreadyCheckedOut);
  baseapp2.reset_checkout();
  ASSERT_TRUE(machined_channel->SendMessage(death).HasValue());

  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  CheckoutEntity co2;
  co2.entity_def_digest = atlas::test_fixtures::kAccountDefinitionDigest;
  co2.owner_process_instance = 111;
  ASSERT_TRUE(FillSecureRandom(co2.operation_id));
  co2.mode = LoadMode::kByDbid;
  co2.type_id = 1;
  co2.dbid = dbid;
  co2.entity_id = 2002;
  co2.request_id = 12;
  co2.owner_addr = baseapp2.local_addr();
  ASSERT_TRUE((*ch2)->SendMessage(co2).HasValue());
  ASSERT_TRUE(poll_until(baseapp2.dispatcher, [&] {
    return baseapp2.checkout_received.load(std::memory_order_acquire);
  }));
  EXPECT_EQ(baseapp2.checkout_ack.status, CheckoutStatus::kSuccess);
  EXPECT_EQ(baseapp2.checkout_ack.dbid, dbid);

  stop_flag.store(true, std::memory_order_release);
  app_thread.join();

  std::filesystem::remove(entity_defs);
}

TEST(DBAppOwnershipIntegration, CheckoutFencesOldWriteCheckinAbortAndCachedReply) {
  ASSERT_TRUE(test_fixtures::ResetMysqlTestDatabase());
  const auto port = reserve_udp_port();
  ASSERT_NE(port, 0u);
  const auto definitions = write_entity_defs_bin();
  std::promise<Address> ready;
  auto address = ready.get_future();
  std::atomic<bool> stop{false};
  DBAppClient machined_client{"session_machined"};
  ASSERT_TRUE(machined_client.network.StartTcpServer(Address("127.0.0.1", 0)));
  Channel* machined_channel = nullptr;
  ASSERT_TRUE(
      machined_client.network.InterfaceTable().RegisterTypedHandler<machined::RegisterMessage>(
          [&](const Address&, Channel* channel, const machined::RegisterMessage&) {
            machined_channel = channel;
            (void)channel->SendMessage(machined::RegisterAck{});
          }));
  std::jthread server([&] {
    EventDispatcher dispatcher{"dbapp_shared"};
    dispatcher.SetMaxPollWait(Milliseconds(1));
    NetworkInterface network(dispatcher);
    TestDBApp app(dispatcher, network, ready, stop);
    DBAppArgv args(port, definitions);
    for (std::size_t i = 0; i + 1 < args.storage.size(); ++i) {
      if (args.storage[i] == "--machined") {
        args.storage[i + 1] = machined_client.network.TcpAddress().ToString();
        args.ptrs[i + 1] = args.storage[i + 1].data();
      }
    }
    EXPECT_EQ(app.RunApp(args.argc(), args.argv()), 0);
  });
  struct StopServer {
    std::atomic<bool>& flag;
    ~StopServer() { flag.store(true, std::memory_order_release); }
  } stop_server{stop};
  const auto server_addr = address.get();
  ASSERT_NE(server_addr.Port(), 0u);
  DBAppClient first{"shared_first"};
  DBAppClient next{"shared_next"};
  first.start_local_rudp(reserve_udp_port());
  next.start_local_rudp(reserve_udp_port());
  auto first_channel = first.network.ConnectRudp(server_addr);
  auto next_channel = next.network.ConnectRudp(server_addr);
  ASSERT_TRUE(first_channel.HasValue());
  ASSERT_TRUE(next_channel.HasValue());
  WriteEntity write;
  write.entity_def_digest = atlas::test_fixtures::kAccountDefinitionDigest;
  write.owner.process_instance = 111;
  ASSERT_TRUE(FillSecureRandom(write.operation_id).HasValue());
  write.type_id = 1;
  write.flags = WriteFlags::kCreateNew;
  write.entity_id = 1001;
  write.owner.app_id = 7;
  write.request_id = 101;
  auto encoded = EncodeDbBlob(make_blob("shared"), {});
  ASSERT_TRUE(encoded);
  write.blob = std::move(*encoded);
  ASSERT_TRUE((*first_channel)->SendMessage(write).HasValue());
  ASSERT_TRUE(poll_until(first.dispatcher, [&] { return first.write_received.load(); }));
  ASSERT_TRUE(first.write_ack.success) << first.write_ack.error;
  const auto dbid = first.write_ack.dbid;
  ASSERT_GT(first.write_ack.owner.ownership_version, 0u);
  const auto first_save_owner = first.write_ack.owner;
  ASSERT_TRUE(poll_until(machined_client.dispatcher, [&] { return machined_channel != nullptr; }));
  machined::BirthNotification birth;
  birth.process_type = ProcessType::kBaseApp;
  birth.process_instance = 111;
  birth.name = "session-first";
  birth.internal_addr = first.local_addr();
  ASSERT_TRUE((*first_channel)->SendMessage(birth));
  AccountSessionRequest probe;
  probe.request_id = (uint64_t{1} << 40) - 1;
  probe.process_instance = 111;
  probe.account_dbid = dbid;
  probe.account_type_id = 1;
  ASSERT_TRUE((*first_channel)->SendMessage(probe));
  ASSERT_TRUE(poll_until(first.dispatcher, [&] { return first.session_reply.has_value(); }));
  EXPECT_EQ(first.session_reply->status, ErrorCode::kPermissionDenied);
  ASSERT_TRUE(machined_channel->SendMessage(birth));
  birth.name = "session-next";
  birth.internal_addr = next.local_addr();
  ASSERT_TRUE(machined_channel->SendMessage(birth));
  ASSERT_TRUE(machined_channel->SendMessage(probe));
  ASSERT_TRUE(poll_until(machined_client.dispatcher,
                         [&] { return machined_client.session_reply.has_value(); }));
  CheckAccountSessionProtocol(first, **first_channel, next, **next_channel, dbid, first_save_owner);
  auto changed_write = write;
  changed_write.request_id = 109;
  changed_write.blob.push_back(std::byte{1});
  first.reset_write();
  ASSERT_TRUE((*first_channel)->SendMessage(changed_write).HasValue());
  ASSERT_TRUE(poll_until(first.dispatcher, [&] { return first.write_received.load(); }));
  EXPECT_FALSE(first.write_ack.success);
  EXPECT_EQ(first.write_ack.error_kind, DatabaseErrorKind::kOperationConflict);
  CheckoutEntity competing;
  competing.entity_def_digest = atlas::test_fixtures::kAccountDefinitionDigest;
  competing.owner_process_instance = 111;
  ASSERT_TRUE(FillSecureRandom(competing.operation_id));
  competing.dbid = dbid;
  competing.type_id = 1;
  competing.entity_id = 2002;
  competing.owner_app_id = 8;
  competing.request_id = 110;
  ASSERT_TRUE((*next_channel)->SendMessage(competing).HasValue());
  ASSERT_TRUE(poll_until(next.dispatcher, [&] { return next.checkout_received.load(); }));
  EXPECT_EQ(next.checkout_ack.status, CheckoutStatus::kAlreadyCheckedOut);
  EXPECT_EQ(next.checkout_ack.holder_entity_id, first_save_owner.entity_id);
  next.reset_checkout();
  write.dbid = dbid;
  write.owner = first_save_owner;
  write.flags = WriteFlags::kLogOff;
  write.request_id = 100;
  ASSERT_TRUE(FillSecureRandom(write.operation_id).HasValue());
  first.reset_write();
  ASSERT_TRUE((*first_channel)->SendMessage(write).HasValue());
  ASSERT_TRUE(poll_until(first.dispatcher, [&] { return first.write_received.load(); }));
  ASSERT_TRUE(first.write_ack.success) << first.write_ack.error;

  CheckoutEntity checkout;
  checkout.entity_def_digest = atlas::test_fixtures::kAccountDefinitionDigest;
  checkout.owner_process_instance = 111;
  ASSERT_TRUE(FillSecureRandom(checkout.operation_id));
  checkout.type_id = 1;
  checkout.dbid = dbid;
  checkout.request_id = 111;
  checkout.entity_id = 1001;
  checkout.owner_app_id = 7;
  ASSERT_TRUE((*first_channel)->SendMessage(checkout).HasValue());
  ASSERT_TRUE(poll_until(first.dispatcher, [&] { return first.checkout_received.load(); }));
  ASSERT_EQ(first.checkout_ack.status, CheckoutStatus::kSuccess);
  ASSERT_GT(first.checkout_ack.checkout_version, 0u);
  const auto original = first.checkout_ack;
  write.dbid = dbid;
  write.flags = WriteFlags::kLogOff;
  write.request_id = 102;
  write.owner = CheckoutInfo{original.holder_addr, original.holder_app_id,
                             original.holder_entity_id, original.checkout_version, 111};
  ASSERT_TRUE(FillSecureRandom(write.operation_id).HasValue());
  first.reset_write();
  ASSERT_TRUE((*first_channel)->SendMessage(write).HasValue());
  ASSERT_TRUE(poll_until(first.dispatcher, [&] { return first.write_received.load(); }));
  ASSERT_TRUE(first.write_ack.success) << first.write_ack.error;
  auto another = checkout;
  ASSERT_TRUE(FillSecureRandom(another.operation_id));
  const auto completed_logoff = write;
  another.request_id = 112;
  another.entity_id = 2002;
  another.owner_app_id = 8;
  ASSERT_TRUE((*next_channel)->SendMessage(another).HasValue());
  ASSERT_TRUE(poll_until(next.dispatcher, [&] { return next.checkout_received.load(); }));
  ASSERT_EQ(next.checkout_ack.status, CheckoutStatus::kSuccess);
  ASSERT_GT(next.checkout_ack.checkout_version, original.checkout_version);
  const auto current = next.checkout_ack;
  auto replayed_logoff = completed_logoff;
  replayed_logoff.request_id = 117;
  first.reset_write();
  ASSERT_TRUE((*first_channel)->SendMessage(replayed_logoff).HasValue());
  ASSERT_TRUE(poll_until(first.dispatcher, [&] { return first.write_received.load(); }));
  EXPECT_TRUE(first.write_ack.success) << first.write_ack.error;
  write.request_id = 103;
  ASSERT_TRUE(FillSecureRandom(write.operation_id).HasValue());
  first.reset_write();
  ASSERT_TRUE((*first_channel)->SendMessage(write).HasValue());
  ASSERT_TRUE(poll_until(first.dispatcher, [&] { return first.write_received.load(); }));
  EXPECT_FALSE(first.write_ack.success);
  CheckinEntity stale_checkin;
  stale_checkin.dbid = dbid;
  stale_checkin.type_id = 1;
  stale_checkin.owner = write.owner;
  ASSERT_TRUE((*first_channel)->SendMessage(stale_checkin).HasValue());
  AbortCheckout stale_abort;
  stale_abort.owner_process_instance = 111;
  stale_abort.operation_id = checkout.operation_id;
  stale_abort.dbid = dbid;
  stale_abort.type_id = 1;
  stale_abort.request_id = checkout.request_id;
  stale_abort.mode = checkout.mode;
  stale_abort.identifier = checkout.identifier;
  stale_abort.entity_id = checkout.entity_id;
  stale_abort.owner_app_id = checkout.owner_app_id;
  stale_abort.owner_process_instance = checkout.owner_process_instance;
  ASSERT_TRUE((*first_channel)->SendMessage(stale_abort).HasValue());
  ASSERT_TRUE(poll_until(first.dispatcher, [&] { return first.abort_received.load(); }));
  EXPECT_TRUE(first.abort_ack.success);
  EXPECT_EQ(first.abort_ack.operation_id, checkout.operation_id);
  first.reset_checkout();
  ASSERT_TRUE((*first_channel)->SendMessage(checkout).HasValue());
  ASSERT_TRUE(poll_until(first.dispatcher, [&] { return first.checkout_received.load(); }));
  EXPECT_EQ(first.checkout_ack.status, CheckoutStatus::kDbError);
  EXPECT_EQ(first.checkout_ack.error, "checkout_lease_expired");
  write.flags = WriteFlags::kNone;
  write.owner = CheckoutInfo{current.holder_addr, current.holder_app_id, current.holder_entity_id,
                             current.checkout_version, 111};
  write.request_id = 104;
  ASSERT_TRUE(FillSecureRandom(write.operation_id).HasValue());
  next.reset_write();
  ASSERT_TRUE((*next_channel)->SendMessage(write).HasValue());
  ASSERT_TRUE(poll_until(next.dispatcher, [&] { return next.write_received.load(); }));
  EXPECT_TRUE(next.write_ack.success) << next.write_ack.error;
  CheckinEntity confirmed_checkin;
  confirmed_checkin.request_id = 901;
  confirmed_checkin.dbid = dbid;
  confirmed_checkin.type_id = 1;
  confirmed_checkin.owner = write.owner;
  next.drop_checkin_reply = true;
  ASSERT_TRUE((*next_channel)->SendMessage(confirmed_checkin));
  ASSERT_TRUE(poll_until(next.dispatcher, [&] { return next.dropped_checkin_count == 1; }));
  ASSERT_TRUE(next.checkin_ack.success) << next.checkin_ack.error;
  next.drop_checkin_reply = false;
  ASSERT_TRUE((*next_channel)->SendMessage(confirmed_checkin));
  ASSERT_TRUE(poll_until(next.dispatcher, [&] { return next.checkin_received.load(); }));
  EXPECT_TRUE(next.checkin_ack.success) << next.checkin_ack.error;
  EXPECT_EQ(next.checkin_ack.request.owner, write.owner);
  auto reacquire = another;
  ASSERT_TRUE(FillSecureRandom(reacquire.operation_id));
  reacquire.request_id = 902;
  next.reset_checkout();
  ASSERT_TRUE((*next_channel)->SendMessage(reacquire));
  ASSERT_TRUE(poll_until(next.dispatcher, [&] { return next.checkout_received.load(); }));
  ASSERT_EQ(next.checkout_ack.status, CheckoutStatus::kSuccess);
  EXPECT_GT(next.checkout_ack.checkout_version, write.owner.ownership_version);
  write.owner.ownership_version = next.checkout_ack.checkout_version;
  next.checkin_received = false;
  ASSERT_TRUE((*next_channel)->SendMessage(confirmed_checkin));
  ASSERT_TRUE(poll_until(next.dispatcher, [&] { return next.checkin_received.load(); }));
  EXPECT_TRUE(next.checkin_ack.success);
  DeleteEntity removal;
  removal.dbid = dbid;
  removal.type_id = 1;
  removal.request_id = 115;
  removal.owner = stale_checkin.owner;
  ASSERT_TRUE(FillSecureRandom(removal.operation_id).HasValue());
  ASSERT_TRUE((*first_channel)->SendMessage(removal).HasValue());
  ASSERT_TRUE(poll_until(first.dispatcher, [&] { return first.delete_received.load(); }));
  EXPECT_FALSE(first.delete_ack.success);
  removal.owner = write.owner;
  removal.request_id = 116;
  ASSERT_TRUE(FillSecureRandom(removal.operation_id).HasValue());
  ASSERT_TRUE((*next_channel)->SendMessage(removal).HasValue());
  ASSERT_TRUE(poll_until(next.dispatcher, [&] { return next.delete_received.load(); }));
  EXPECT_TRUE(next.delete_ack.success) << next.delete_ack.error;
  next.delete_received.store(false);
  ASSERT_TRUE((*next_channel)->SendMessage(removal).HasValue());
  ASSERT_TRUE(poll_until(next.dispatcher, [&] { return next.delete_received.load(); }));
  EXPECT_TRUE(next.delete_ack.success) << next.delete_ack.error;
}

TEST(DBAppIntegration, LostCheckoutReplyRetriesThroughAnotherDbapp) {
  ASSERT_TRUE(test_fixtures::ResetMysqlTestDatabase());
  if (std::getenv("ATLAS_TEST_MYSQL") == nullptr) {
    FAIL() << "MySQL environment blocked: isolated ATLAS_TEST_MYSQL configuration required";
  }
  for (const char* name :
       {"ATLAS_TEST_MYSQL_HOST", "ATLAS_TEST_MYSQL_PORT", "ATLAS_TEST_MYSQL_USER",
        "ATLAS_TEST_MYSQL_PASSWORD", "ATLAS_TEST_MYSQL_DATABASE"}) {
    ASSERT_NE(std::getenv(name), nullptr) << "missing " << name;
  }
  const auto definitions = write_entity_defs_bin();
  const auto executable = fs::ExecutablePath();
  ASSERT_TRUE(executable);
#if defined(_WIN32)
  const auto helper = executable->parent_path() / "dbapp_test_process.exe";
#else
  const auto helper = executable->parent_path() / "dbapp_test_process";
#endif
  const Address first_addr("127.0.0.1", reserve_udp_port());
  const Address second_addr("127.0.0.1", reserve_udp_port());
  auto launch = [&](const Address& address) {
    ChildProcess::Options options;
    options.exe = helper;
    options.args = {"--type",
                    "dbapp",
                    "--name",
                    "checkout_process",
                    "--db-type",
                    "mysql",
                    "--machined",
                    "127.0.0.1:1",
                    "--internal-port",
                    std::to_string(address.Port()),
                    "--entitydef-bin-path",
                    definitions.string(),
                    "--account-type-id",
                    "1",
                    "--db-mysql-database-id",
                    "0123456789abcdef0123456789abcdef",
                    "--db-mysql-initialize",
                    "true"};
    return ChildProcess::Start(std::move(options));
  };
  auto first_server = launch(first_addr);
  ASSERT_TRUE(first_server);
  auto ready = [](ChildProcess& process) {
    std::string transcript;
    const auto deadline = Clock::now() + std::chrono::seconds(60);
    while (Clock::now() < deadline) {
      if (auto line = process.WaitForStdoutLine(Milliseconds(100))) {
        if (*line == "DBAPP_TEST_READY") return true;
        transcript += *line + "\n";
      } else if (!process.IsRunning()) {
        break;
      }
    }
    ADD_FAILURE() << "DBApp subprocess failed to start:\n" << transcript;
    return false;
  };
  ASSERT_TRUE(ready(*first_server));
  auto second_server = launch(second_addr);
  ASSERT_TRUE(second_server);
  ASSERT_TRUE(ready(*second_server));
  ASSERT_NE(first_addr, second_addr);
  DBAppClient client{"checkout_retry_client"};
  client.start_local_rudp(reserve_udp_port());
  auto first_channel = client.network.ConnectRudp(first_addr);
  ASSERT_TRUE(first_channel);
  auto parallel_channel = client.network.ConnectRudp(second_addr);
  ASSERT_TRUE(parallel_channel);
  std::vector<std::pair<Address, GetEntityIdsAck>> reservations;
  ASSERT_TRUE(client.network.InterfaceTable().RegisterTypedHandler<GetEntityIdsAck>(
      [&](const Address& source, Channel*, const GetEntityIdsAck& message) {
        reservations.emplace_back(source, message);
      }));
  GetEntityIds reserve;
  reserve.count = 32;
  ASSERT_TRUE((*first_channel)->SendMessage(reserve));
  ASSERT_TRUE((*parallel_channel)->SendMessage(reserve));
  ASSERT_TRUE(poll_until(client.dispatcher, [&] { return reservations.size() == 2; }));
  ASSERT_NE(reservations[0].first, reservations[1].first);
  for (const auto& [source, range] : reservations) {
    EXPECT_GT(range.start, 0u);
    EXPECT_EQ(range.count, 32u);
    EXPECT_EQ(range.end - range.start + 1, 32u);
  }
  EXPECT_TRUE(reservations[0].second.end < reservations[1].second.start ||
              reservations[1].second.end < reservations[0].second.start);
  WriteEntity write;
  write.entity_def_digest = atlas::test_fixtures::kAccountDefinitionDigest;
  write.owner.process_instance = 111;
  ASSERT_TRUE(FillSecureRandom(write.operation_id));
  write.type_id = 1;
  write.flags = WriteFlags::kCreateNew;
  write.entity_id = 1001;
  write.owner.app_id = 7;
  write.request_id = 1;
  write.identifier = "cross_dbapp_checkout_retry";
  auto encoded = EncodeDbBlob(make_blob("original"), {});
  ASSERT_TRUE(encoded);
  write.blob = std::move(*encoded);
  ASSERT_TRUE((*first_channel)->SendMessage(write));
  ASSERT_TRUE(poll_until(client.dispatcher, [&] { return client.write_received.load(); }));
  ASSERT_TRUE(client.write_ack.success) << client.write_ack.error;
  const auto dbid = client.write_ack.dbid;
  write.dbid = dbid;
  write.flags = WriteFlags::kLogOff;
  write.owner = client.write_ack.owner;
  write.request_id = 2;
  ASSERT_TRUE(FillSecureRandom(write.operation_id));
  client.reset_write();
  ASSERT_TRUE((*first_channel)->SendMessage(write));
  ASSERT_TRUE(poll_until(client.dispatcher, [&] { return client.write_received.load(); }));
  ASSERT_TRUE(client.write_ack.success) << client.write_ack.error;
  CheckoutEntity checkout;
  checkout.entity_def_digest = atlas::test_fixtures::kAccountDefinitionDigest;
  checkout.owner_process_instance = 111;
  ASSERT_TRUE(FillSecureRandom(checkout.operation_id));
  checkout.dbid = dbid;
  checkout.type_id = 1;
  checkout.request_id = 3;
  checkout.owner_app_id = 7;
  checkout.entity_id = 1001;
  client.drop_checkout_reply = true;
  ASSERT_TRUE((*first_channel)->SendMessage(checkout));
  ASSERT_TRUE(poll_until(client.dispatcher, [&] { return client.dropped_checkout_count == 1; }));
  ASSERT_FALSE(client.checkout_received.load());
  const auto original = client.checkout_ack;
  ASSERT_EQ(original.status, CheckoutStatus::kSuccess);
  ASSERT_GT(original.checkout_version, 0u);
  auto canceled_write = write;
  canceled_write.request_id = 10;
  canceled_write.flags = WriteFlags::kCreateNew;
  canceled_write.dbid = kInvalidDBID;
  canceled_write.entity_id = 1002;
  canceled_write.owner = CheckoutInfo{};
  canceled_write.owner.process_instance = 111;
  canceled_write.owner.app_id = 7;
  canceled_write.identifier = "cross_dbapp_checkout_cancel";
  ASSERT_TRUE(FillSecureRandom(canceled_write.operation_id));
  client.reset_write();
  ASSERT_TRUE((*first_channel)->SendMessage(canceled_write));
  ASSERT_TRUE(poll_until(client.dispatcher, [&] { return client.write_received.load(); }));
  ASSERT_TRUE(client.write_ack.success) << client.write_ack.error;
  canceled_write.dbid = client.write_ack.dbid;
  canceled_write.owner = client.write_ack.owner;
  canceled_write.flags = WriteFlags::kLogOff;
  canceled_write.request_id = 11;
  ASSERT_TRUE(FillSecureRandom(canceled_write.operation_id));
  client.reset_write();
  ASSERT_TRUE((*first_channel)->SendMessage(canceled_write));
  ASSERT_TRUE(poll_until(client.dispatcher, [&] { return client.write_received.load(); }));
  ASSERT_TRUE(client.write_ack.success) << client.write_ack.error;
  auto canceled_checkout = checkout;
  canceled_checkout.dbid = canceled_write.dbid;
  canceled_checkout.entity_id = 1002;
  canceled_checkout.request_id = 12;
  ASSERT_TRUE(FillSecureRandom(canceled_checkout.operation_id));
  ASSERT_TRUE((*first_channel)->SendMessage(canceled_checkout));
  ASSERT_TRUE(poll_until(client.dispatcher, [&] { return client.dropped_checkout_count == 2; }));
  ASSERT_EQ(client.checkout_ack.status, CheckoutStatus::kSuccess);
  const auto canceled_version = client.checkout_ack.checkout_version;
  first_server->Kill();
  ASSERT_TRUE(first_server->Wait(std::chrono::seconds(10)).has_value());
  client.network.DisconnectChannel(first_addr);
  auto second_channel = client.network.ConnectRudp(second_addr);
  ASSERT_TRUE(second_channel);
  client.drop_checkout_reply = false;
  checkout.request_id = 4;
  ASSERT_TRUE((*second_channel)->SendMessage(checkout));
  ASSERT_TRUE(poll_until(client.dispatcher, [&] { return client.checkout_received.load(); }));
  ASSERT_EQ(client.checkout_ack.status, CheckoutStatus::kSuccess) << client.checkout_ack.error;
  EXPECT_EQ(client.checkout_ack.dbid, original.dbid);
  EXPECT_EQ(client.checkout_ack.checkout_version, original.checkout_version);
  EXPECT_EQ(client.checkout_ack.blob, original.blob);
  EXPECT_EQ(client.checkout_ack.holder_addr, client.local_addr());
  AbortCheckout abort;
  abort.owner_process_instance = 111;
  abort.operation_id = canceled_checkout.operation_id;
  abort.request_id = canceled_checkout.request_id;
  abort.type_id = canceled_checkout.type_id;
  abort.dbid = canceled_checkout.dbid;
  abort.mode = canceled_checkout.mode;
  abort.identifier = canceled_checkout.identifier;
  abort.entity_id = canceled_checkout.entity_id;
  abort.owner_app_id = canceled_checkout.owner_app_id;
  abort.owner_process_instance = canceled_checkout.owner_process_instance;
  client.abort_received.store(false);
  ASSERT_TRUE((*second_channel)->SendMessage(abort));
  ASSERT_TRUE(poll_until(client.dispatcher, [&] { return client.abort_received.load(); }));
  ASSERT_TRUE(client.abort_ack.success);
  EXPECT_EQ(client.abort_ack.operation_id, abort.operation_id);
  client.abort_received.store(false);
  ASSERT_TRUE((*second_channel)->SendMessage(abort));
  ASSERT_TRUE(poll_until(client.dispatcher, [&] { return client.abort_received.load(); }));
  EXPECT_TRUE(client.abort_ack.success);
  ASSERT_TRUE(FillSecureRandom(abort.operation_id));
  abort.request_id = 13;
  client.abort_received.store(false);
  ASSERT_TRUE((*second_channel)->SendMessage(abort));
  ASSERT_TRUE(poll_until(client.dispatcher, [&] { return client.abort_received.load(); }));
  EXPECT_FALSE(client.abort_ack.success);
  auto next_checkout = canceled_checkout;
  next_checkout.request_id = 14;
  ASSERT_TRUE(FillSecureRandom(next_checkout.operation_id));
  client.reset_checkout();
  ASSERT_TRUE((*second_channel)->SendMessage(next_checkout));
  ASSERT_TRUE(poll_until(client.dispatcher, [&] { return client.checkout_received.load(); }));
  ASSERT_EQ(client.checkout_ack.status, CheckoutStatus::kSuccess) << client.checkout_ack.error;
  EXPECT_EQ(client.checkout_ack.checkout_version, canceled_version + 1);
  canceled_write.owner =
      CheckoutInfo{client.checkout_ack.holder_addr, client.checkout_ack.holder_app_id,
                   client.checkout_ack.holder_entity_id, client.checkout_ack.checkout_version, 111};
  canceled_write.request_id = 15;
  ASSERT_TRUE(FillSecureRandom(canceled_write.operation_id));
  client.reset_write();
  ASSERT_TRUE((*second_channel)->SendMessage(canceled_write));
  ASSERT_TRUE(poll_until(client.dispatcher, [&] { return client.write_received.load(); }));
  ASSERT_TRUE(client.write_ack.success) << client.write_ack.error;
  write.owner = CheckoutInfo{original.holder_addr, original.holder_app_id,
                             original.holder_entity_id, original.checkout_version, 111};
  write.request_id = 5;
  ASSERT_TRUE(FillSecureRandom(write.operation_id));
  client.reset_write();
  ASSERT_TRUE((*second_channel)->SendMessage(write));
  ASSERT_TRUE(poll_until(client.dispatcher, [&] { return client.write_received.load(); }));
  ASSERT_TRUE(client.write_ack.success) << client.write_ack.error;
  checkout.request_id = 6;
  client.reset_checkout();
  ASSERT_TRUE((*second_channel)->SendMessage(checkout));
  ASSERT_TRUE(poll_until(client.dispatcher, [&] { return client.checkout_received.load(); }));
  EXPECT_EQ(client.checkout_ack.status, CheckoutStatus::kDbError);
  EXPECT_EQ(client.checkout_ack.error, "checkout_lease_expired");
}
