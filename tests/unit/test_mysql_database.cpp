#define _CRT_SECURE_NO_WARNINGS  // std::getenv is flagged unsafe by MSVC

#include <mysql.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "atdf_account_fixture.h"
#include "db_mysql/mysql_database.h"
#include "entitydef/entity_def_registry.h"
#include "network/socket.h"
#include "test_account_session_contract.h"
#include "test_database_admission.h"

using namespace atlas;

namespace {

auto env_or(const char* key, const char* fallback) -> std::string {
  const char* v = std::getenv(key);
  return v != nullptr ? std::string(v) : std::string(fallback);
}

auto make_blob(std::string_view text) -> std::vector<std::byte> {
  std::vector<std::byte> blob;
  blob.reserve(text.size());
  for (char ch : text) {
    blob.push_back(static_cast<std::byte>(ch));
  }
  return blob;
}

void register_account_entity() {
  EntityDefRegistry::Instance().clear();
  const auto blob = atlas::test_fixtures::MakeAccountTypeBlob();
  ASSERT_TRUE(
      EntityDefRegistry::Instance().RegisterType(blob.data(), static_cast<int32_t>(blob.size())));
}

auto make_config() -> DatabaseConfig {
  DatabaseConfig cfg;
  cfg.type = "mysql";
  cfg.mysql_host = env_or("ATLAS_TEST_MYSQL_HOST", "127.0.0.1");
  cfg.mysql_port = static_cast<uint16_t>(std::stoi(env_or("ATLAS_TEST_MYSQL_PORT", "3306")));
  cfg.mysql_user = env_or("ATLAS_TEST_MYSQL_USER", "root");
  cfg.mysql_password = env_or("ATLAS_TEST_MYSQL_PASSWORD", "");
  cfg.mysql_database = env_or("ATLAS_TEST_MYSQL_DATABASE", "atlas_test");
  return cfg;
}

auto connect_admin(const DatabaseConfig& cfg) -> std::unique_ptr<MYSQL, decltype(&mysql_close)> {
  std::unique_ptr<MYSQL, decltype(&mysql_close)> admin(mysql_init(nullptr), mysql_close);
  if (!admin) return admin;
  unsigned int timeout = 5;
  mysql_options(admin.get(), MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
  if (!mysql_real_connect(admin.get(), cfg.mysql_host.c_str(), cfg.mysql_user.c_str(),
                          cfg.mysql_password.c_str(), cfg.mysql_database.c_str(), cfg.mysql_port,
                          nullptr, 0)) {
    ADD_FAILURE() << "MySQL admin connection failed, error=" << mysql_errno(admin.get()) << ": "
                  << mysql_error(admin.get());
    admin.reset();
  }
  return admin;
}

auto reset_schema(const DatabaseConfig& cfg) -> bool {
  auto admin = connect_admin(cfg);
  if (!admin) return false;
  static constexpr const char* kDrop =
      "DROP TABLE IF EXISTS entities, meta, atlas_entity_id_counter, atlas_write_receipts, "
      "atlas_checkout_versions, atlas_checkout_processes, atlas_account_sessions";
  const bool ok =
      mysql_real_query(admin.get(), kDrop, static_cast<unsigned long>(std::strlen(kDrop))) == 0;
  return ok;
}

auto RelayUntilCommittedReply(Socket listener, const Address& upstream, const DatabaseConfig& cfg,
                              std::stop_token stop, const std::atomic<bool>& armed,
                              std::atomic<bool>& dropped,
                              std::string_view committed_query =
                                  "SELECT COUNT(*) FROM entities e JOIN atlas_write_receipts r "
                                  "ON r.result_dbid=e.dbid WHERE e.identifier='lost_commit_reply'")
    -> Result<void> {
  auto admin = connect_admin(cfg);
  if (!admin) return Error{ErrorCode::kIoError, "proxy observer connection failed"};
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  auto running = [&] {
    return !stop.stop_requested() && std::chrono::steady_clock::now() < deadline;
  };
  std::optional<Socket> client;
  while (running() && !client) {
    auto accepted = listener.Accept();
    if (accepted)
      client.emplace(std::move(accepted->first));
    else if (accepted.Error().Code() != ErrorCode::kWouldBlock)
      return accepted.Error();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  if (!client) return Error{ErrorCode::kTimeout, "proxy accept did not complete"};
  auto server = Socket::CreateTcp();
  if (!server) return server.Error();
  if (auto mode = server->SetNonBlocking(false); !mode) return mode.Error();
  if (auto connected = server->Connect(upstream); !connected) return connected.Error();
  if (auto mode = server->SetNonBlocking(true); !mode) return mode.Error();
  auto relay = [&](Socket& from, Socket& to, bool server_reply) -> Result<void> {
    std::array<std::byte, 16384> bytes;
    auto read = from.Recv(bytes);
    if (!read) {
      if (read.Error().Code() == ErrorCode::kWouldBlock) return {};
      return read.Error();
    }
    if (*read == 0) return Error{ErrorCode::kIoError, "proxy connection closed before fault"};
    if (server_reply && armed.load()) {
      if (mysql_real_query(admin.get(), committed_query.data(),
                           static_cast<unsigned long>(committed_query.size())) != 0)
        return Error{ErrorCode::kIoError, "proxy commit observer query failed"};
      std::unique_ptr<MYSQL_RES, decltype(&mysql_free_result)> rows(mysql_store_result(admin.get()),
                                                                    mysql_free_result);
      if (!rows) return Error{ErrorCode::kIoError, "proxy commit observer result missing"};
      auto row = mysql_fetch_row(rows.get());
      if (row && row[0] && std::string_view(row[0]) == "1") {
        dropped.store(true);
        return {};
      }
    }
    std::size_t offset = 0;
    while (running() && offset < *read) {
      auto written = to.Send(std::span(bytes).subspan(offset, *read - offset));
      if (written && *written != 0)
        offset += *written;
      else if (!written && written.Error().Code() == ErrorCode::kWouldBlock)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      else
        return Error{ErrorCode::kIoError, "proxy forwarding failed"};
    }
    return {};
  };
  while (running()) {
    if (auto result = relay(*client, *server, false); !result) return result.Error();
    if (auto result = relay(*server, *client, true); !result) return result.Error();
    if (dropped.load()) return {};
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return Error{ErrorCode::kTimeout, "proxy stopped before committed reply"};
}

}  // namespace

namespace atlas {

struct MysqlDatabaseTestAccess {
  static auto Begin(MysqlDatabase& db) -> Result<void> { return db.BeginTxn(db.conn_); }
  static auto Commit(MysqlDatabase& db) -> Result<void> { return db.Commit(db.conn_); }
  static auto ConnectionId(MysqlDatabase& db) -> unsigned long { return mysql_thread_id(db.conn_); }
  static auto UsesTls(MysqlDatabase& db) -> bool {
    return mysql_get_ssl_cipher(db.conn_) != nullptr;
  }
};

}  // namespace atlas

TEST(MysqlConfiguration, MissingSettingsFailBeforeConnecting) {
  MysqlDatabase database;
  DatabaseConfig config;
  auto result = database.Startup(config, EntityDefRegistry::Instance());
  ASSERT_FALSE(result.HasValue());
  EXPECT_EQ(result.Error().Code(), ErrorCode::kInvalidArgument);
  EXPECT_NE(result.Error().Message().find("SQLite fallback is disabled"), std::string_view::npos);
}

class MysqlDatabaseTest : public ::testing::Test {
 protected:
  void SetUp() override {
    if (std::getenv("ATLAS_TEST_MYSQL") == nullptr) {
      FAIL() << "MySQL environment blocked: set ATLAS_TEST_MYSQL and all connection settings";
    }
    for (const char* key :
         {"ATLAS_TEST_MYSQL_HOST", "ATLAS_TEST_MYSQL_PORT", "ATLAS_TEST_MYSQL_USER",
          "ATLAS_TEST_MYSQL_PASSWORD", "ATLAS_TEST_MYSQL_DATABASE"}) {
      const char* value = std::getenv(key);
      ASSERT_NE(value, nullptr) << "MySQL environment blocked: missing " << key;
      ASSERT_NE(*value, '\0') << "MySQL environment blocked: empty " << key;
    }
    register_account_entity();

    cfg_ = make_config();
    ASSERT_TRUE(reset_schema(cfg_))
        << "cannot prepare isolated MySQL schema at " << cfg_.mysql_host << ":" << cfg_.mysql_port;

    auto start = db_.Startup(cfg_, EntityDefRegistry::Instance());
    ASSERT_TRUE(start.HasValue()) << "db startup failed: " << start.Error().Message();
  }

  void TearDown() override {
    db_.Shutdown();
    EntityDefRegistry::Instance().clear();
  }

  // Deferred-mode results arrive off the worker pool, so pump ProcessResults
  // until the callback fires (or the timeout trips).
  void pump_until(const std::function<bool()>& done, int timeout_ms = 5000) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (!done() && std::chrono::steady_clock::now() < deadline) {
      db_.ProcessResults();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  MysqlDatabase db_;
  DatabaseConfig cfg_;
};

TEST_F(MysqlDatabaseTest, StartupRejectsSchemaVersionWithoutOverwritingIt) {
  auto admin = connect_admin(cfg_);
  ASSERT_TRUE(admin);
  ASSERT_EQ(mysql_query(admin.get(),
                        "UPDATE meta SET `value` = 'unsupported' WHERE `key` = 'schema_version'"),
            0);
  db_.Shutdown();

  auto started = db_.Startup(cfg_, EntityDefRegistry::Instance());
  EXPECT_FALSE(started.HasValue());
  if (!started) {
    EXPECT_NE(started.Error().Message().find("schema_version"), std::string_view::npos);
  }
  db_.Shutdown();
  ASSERT_EQ(mysql_query(admin.get(), "SELECT `value` FROM meta WHERE `key` = 'schema_version'"), 0);
  std::unique_ptr<MYSQL_RES, decltype(&mysql_free_result)> rows(mysql_store_result(admin.get()),
                                                                mysql_free_result);
  ASSERT_TRUE(rows);
  auto row = mysql_fetch_row(rows.get());
  ASSERT_NE(row, nullptr);
  EXPECT_STREQ(row[0], "unsupported");
}

TEST_F(MysqlDatabaseTest, StartupRejectsMissingSchemaVersion) {
  auto admin = connect_admin(cfg_);
  ASSERT_TRUE(admin);
  ASSERT_EQ(mysql_query(admin.get(), "DELETE FROM meta WHERE `key` = 'schema_version'"), 0);
  db_.Shutdown();
  auto started = db_.Startup(cfg_, EntityDefRegistry::Instance());
  ASSERT_FALSE(started.HasValue());
  EXPECT_NE(started.Error().Message().find("schema_version"), std::string_view::npos);
}

TEST_F(MysqlDatabaseTest, StartupRejectsUnsupportedBlobFormatWithoutUpdatingMetadata) {
  auto admin = connect_admin(cfg_);
  ASSERT_TRUE(admin);
  ASSERT_EQ(mysql_query(admin.get(),
                        "UPDATE meta SET `value` = 'unsupported' "
                        "WHERE `key` = 'blob_format_version'"),
            0);
  db_.Shutdown();
  auto started = db_.Startup(cfg_, EntityDefRegistry::Instance());
  ASSERT_FALSE(started.HasValue());
  EXPECT_NE(started.Error().Message().find("blob_format_version"), std::string_view::npos);
}

TEST_F(MysqlDatabaseTest, StartupRejectsChangedSnapshotSchemaWithoutReplacingTheDigest) {
  db_.Shutdown();
  auto& registry = EntityDefRegistry::Instance();
  const auto original = registry.types[0].properties[1].data_type;
  registry.types[0].properties[1].data_type = PropertyDataType::kUInt64;
  auto started = db_.Startup(cfg_, registry);
  EXPECT_FALSE(started.HasValue());
  if (!started) {
    EXPECT_NE(started.Error().Message().find("storage_schema_digest"), std::string_view::npos);
  }
  db_.Shutdown();
  registry.types[0].properties[1].data_type = original;
  ASSERT_TRUE(db_.Startup(cfg_, registry));
}

TEST_F(MysqlDatabaseTest, StartupRejectsMissingSnapshotSchemaDigest) {
  auto admin = connect_admin(cfg_);
  ASSERT_TRUE(admin);
  ASSERT_EQ(mysql_query(admin.get(), "DELETE FROM meta WHERE `key` = 'storage_schema_digest'"), 0);
  db_.Shutdown();
  auto started = db_.Startup(cfg_, EntityDefRegistry::Instance());
  ASSERT_FALSE(started.HasValue());
  EXPECT_NE(started.Error().Message().find("storage_schema_digest"), std::string_view::npos);
}

TEST_F(MysqlDatabaseTest, ConcurrentStartupInitializesOneSchemaAndDisjointIdRanges) {
  db_.Shutdown();
  ASSERT_TRUE(reset_schema(cfg_));
  MysqlDatabase peer;
  Result<void> first;
  Result<void> second;
  std::barrier ready(2);
  std::jthread other([&] {
    ready.arrive_and_wait();
    second = peer.Startup(cfg_, EntityDefRegistry::Instance());
  });
  ready.arrive_and_wait();
  first = db_.Startup(cfg_, EntityDefRegistry::Instance());
  other.join();
  ASSERT_TRUE(first.HasValue()) << first.Error().Message();
  ASSERT_TRUE(second.HasValue()) << second.Error().Message();
  EntityIdRange first_range;
  EntityIdRange second_range;
  db_.ReserveEntityIds(10, [&](Result<EntityIdRange> result) {
    ASSERT_TRUE(result.HasValue());
    first_range = *result;
  });
  peer.ReserveEntityIds(10, [&](Result<EntityIdRange> result) {
    ASSERT_TRUE(result.HasValue());
    second_range = *result;
  });
  EXPECT_TRUE(first_range.last < second_range.first || second_range.last < first_range.first);
  peer.Shutdown();
}

TEST_F(MysqlDatabaseTest, AdmissionIncludesUnconsumedResultsAndRejectsBeforeWriting) {
  atlas::test_support::CheckDatabaseAdmission(db_);
}

TEST_F(MysqlDatabaseTest, DrainRejectsNewWorkAndCompletesAcceptedCallbacks) {
  atlas::test_support::CheckDatabaseDrain(db_);
}

TEST_F(MysqlDatabaseTest, AccountSessionOwnershipContract) {
  atlas::test_support::CheckAccountSessionContract(db_);
}

TEST_F(MysqlDatabaseTest, SessionClaimAcceptsAProcessCheckoutBeforeEntityAllocation) {
  auto claim = atlas::test_support::CreateSessionAccount(db_, 0);
  ASSERT_TRUE(claim.HasValue());
  auto granted = atlas::test_support::UpdateSession(db_, *claim);
  ASSERT_TRUE(granted.HasValue()) << granted.Error().Message();
  EXPECT_EQ(granted->owner.entity_id, 100u);
  db_.GetEntity(claim->account_dbid, 1, [](GetResult result) {
    ASSERT_TRUE(result.success);
    ASSERT_TRUE(result.checked_out_by.has_value());
    EXPECT_EQ(result.checked_out_by->entity_id, 0u);
  });
}

TEST_F(MysqlDatabaseTest, AccountSessionMustBeReleasedBeforeDeletingAccount) {
  atlas::test_support::CheckSessionDeletionFence(db_);
}

TEST_F(MysqlDatabaseTest, AccountSessionAbortFencesLateCompletion) {
  atlas::test_support::CheckSessionAbortContract(db_);
}

TEST_F(MysqlDatabaseTest, AccountSessionRetirementRejectsReceiptReplay) {
  atlas::test_support::CheckRetiredSessionReplay(db_);
}

TEST_F(MysqlDatabaseTest, AccountSessionReceiptFailureRollsBackGrant) {
  auto claim = atlas::test_support::CreateSessionAccount(db_);
  ASSERT_TRUE(claim.HasValue());
  auto admin = connect_admin(cfg_);
  ASSERT_NE(admin, nullptr);
  ASSERT_EQ(
      mysql_query(admin.get(),
                  "CREATE TRIGGER reject_session_receipt BEFORE UPDATE ON atlas_write_receipts "
                  "FOR EACH ROW SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='session receipt fault'"),
      0);
  auto failed = atlas::test_support::UpdateSession(db_, *claim);
  ASSERT_FALSE(failed.HasValue());
  db_.ReadAccountSession(claim->account_dbid, 1, [](Result<AccountSession> value) {
    ASSERT_TRUE(value.HasValue());
    EXPECT_EQ(value->version, 0u);
  });
  ASSERT_EQ(mysql_query(admin.get(), "DROP TRIGGER reject_session_receipt"), 0);
  auto replay = atlas::test_support::UpdateSession(db_, *claim);
  ASSERT_TRUE(replay.HasValue());
  EXPECT_EQ(replay->version, 1u);
}

TEST_F(MysqlDatabaseTest, AccountSessionConcurrentChangesHaveOneWinner) {
  auto claim = atlas::test_support::CreateSessionAccount(db_);
  ASSERT_TRUE(claim.HasValue());
  auto active = atlas::test_support::UpdateSession(db_, *claim);
  ASSERT_TRUE(active.HasValue());
  MysqlDatabase peer;
  ASSERT_TRUE(peer.Startup(cfg_, EntityDefRegistry::Instance()));
  auto prepare = *claim;
  prepare.id[0] = std::byte{2};
  prepare.claim_checkout.reset();
  prepare.expected_version = active->version;
  prepare.action = AccountSessionAction::kPrepare;
  prepare.target = {Address(0x7F000001u, 32000), 200, 202};
  prepare.handoff_id = prepare.id;
  prepare.credential_hash[0] = std::byte{1};
  auto release = prepare;
  release.id[0] = std::byte{3};
  release.action = AccountSessionAction::kRelease;
  std::optional<Result<AccountSession>> first;
  std::optional<Result<AccountSession>> second;
  db_.SetDeferredMode(true);
  peer.SetDeferredMode(true);
  db_.UpdateAccountSession(prepare,
                           [&](Result<AccountSession> value) { first = std::move(value); });
  peer.UpdateAccountSession(release,
                            [&](Result<AccountSession> value) { second = std::move(value); });
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while ((!first || !second) && std::chrono::steady_clock::now() < deadline) {
    db_.ProcessResults();
    peer.ProcessResults();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  EXPECT_NE(first->HasValue(), second->HasValue());
  const auto& rejected = first->HasValue() ? *second : *first;
  ASSERT_FALSE(rejected.HasValue());
  EXPECT_EQ(rejected.Error().Code(), ErrorCode::kPermissionDenied);
  peer.Shutdown();
}

TEST_F(MysqlDatabaseTest, AccountSessionLostCommitReplyCanResolveAcrossInstancesAndRestart) {
  auto claim = atlas::test_support::CreateSessionAccount(db_);
  ASSERT_TRUE(claim.HasValue());
  auto upstream = Address::Resolve(cfg_.mysql_host, cfg_.mysql_port);
  ASSERT_TRUE(upstream.HasValue());
  auto listener = Socket::CreateTcp();
  ASSERT_TRUE(listener.HasValue());
  ASSERT_TRUE(listener->Bind(Address("127.0.0.1", 0)));
  ASSERT_TRUE(listener->Listen());
  auto local = listener->LocalAddress();
  ASSERT_TRUE(local.HasValue());
  std::atomic<bool> armed{false};
  std::atomic<bool> dropped{false};
  std::string proxy_error;
  std::jthread proxy([&, socket = std::move(*listener)](std::stop_token stop) mutable {
    auto result = RelayUntilCommittedReply(
        std::move(socket), *upstream, cfg_, stop, armed, dropped,
        "SELECT COUNT(*) FROM atlas_account_sessions WHERE dbid=10000 AND version=1");
    if (!result) proxy_error = result.Error().Message();
  });
  auto config = cfg_;
  config.mysql_host = "127.0.0.1";
  config.mysql_port = local->Port();
  MysqlDatabase peer;
  ASSERT_TRUE(peer.Startup(config, EntityDefRegistry::Instance()));
  armed.store(true);
  auto unknown = atlas::test_support::UpdateSession(peer, *claim);
  peer.Shutdown();
  proxy.request_stop();
  proxy.join();
  ASSERT_TRUE(proxy_error.empty()) << proxy_error;
  ASSERT_TRUE(dropped.load());
  ASSERT_FALSE(unknown.HasValue());
  EXPECT_EQ(unknown.Error().Code(), ErrorCode::kCommitUnknown);
  auto resolved = atlas::test_support::UpdateSession(db_, *claim);
  ASSERT_TRUE(resolved.HasValue());
  EXPECT_EQ(resolved->owner, claim->actor);
  EXPECT_EQ(resolved->version, 1u);
  db_.Shutdown();
  ASSERT_TRUE(db_.Startup(cfg_, EntityDefRegistry::Instance()));
  resolved = atlas::test_support::UpdateSession(db_, *claim);
  ASSERT_TRUE(resolved.HasValue());
  EXPECT_EQ(resolved->version, 1u);
}

TEST_F(MysqlDatabaseTest, CorruptedAccountSessionIsNotAnOfflineGrant) {
  auto claim = atlas::test_support::CreateSessionAccount(db_);
  ASSERT_TRUE(claim.HasValue());
  ASSERT_TRUE(atlas::test_support::UpdateSession(db_, *claim).HasValue());
  auto admin = connect_admin(cfg_);
  ASSERT_NE(admin, nullptr);
  ASSERT_EQ(mysql_query(admin.get(), "UPDATE atlas_account_sessions SET payload=X'00'"), 0);
  db_.ReadAccountSession(claim->account_dbid, 1, [](Result<AccountSession> value) {
    ASSERT_FALSE(value.HasValue());
    EXPECT_EQ(value.Error().Code(), ErrorCode::kIoError);
  });
  EXPECT_FALSE(atlas::test_support::UpdateSession(db_, *claim).HasValue());
}

TEST_F(MysqlDatabaseTest, PutNewEntityAndGet) {
  PutResult put;
  db_.PutEntity(kInvalidDBID, 1, WriteFlags::kCreateNew, make_blob("hello"), "alice",
                [&](PutResult r) { put = std::move(r); });

  ASSERT_TRUE(put.success) << put.error;
  EXPECT_GT(put.dbid, 0);

  GetResult get;
  db_.GetEntity(put.dbid, 1, [&](GetResult r) { get = std::move(r); });
  ASSERT_TRUE(get.success);
  EXPECT_EQ(get.data.blob, make_blob("hello"));
  EXPECT_EQ(get.data.identifier, "alice");
}

TEST_F(MysqlDatabaseTest, PutNewEntityWithExplicitDbidAndGet) {
  constexpr DatabaseID kDbid = 5000;

  PutResult put;
  db_.PutEntity(kDbid, 1, WriteFlags::kCreateNew | WriteFlags::kExplicitDbid, make_blob("explicit"),
                "alice", [&](PutResult r) { put = std::move(r); });

  ASSERT_TRUE(put.success) << put.error;
  EXPECT_EQ(put.dbid, kDbid);

  GetResult get;
  db_.GetEntity(kDbid, 1, [&](GetResult r) { get = std::move(r); });
  ASSERT_TRUE(get.success);
  EXPECT_EQ(get.data.blob, make_blob("explicit"));

  PutResult duplicate;
  db_.PutEntity(kDbid, 1, WriteFlags::kCreateNew | WriteFlags::kExplicitDbid,
                make_blob("duplicate"), "alice_duplicate",
                [&](PutResult r) { duplicate = std::move(r); });
  EXPECT_FALSE(duplicate.success);
  EXPECT_EQ(duplicate.error_kind, DatabaseErrorKind::kDuplicateDbid);

  PutResult auto_put;
  db_.PutEntity(kInvalidDBID, 1, WriteFlags::kCreateNew, make_blob("auto"), "bob",
                [&](PutResult r) { auto_put = std::move(r); });
  ASSERT_TRUE(auto_put.success) << auto_put.error;
  EXPECT_GT(auto_put.dbid, kDbid);
}

TEST_F(MysqlDatabaseTest, RetiredProcessCannotAcquireOrClearReusedAddressOwner) {
  PutResult created;
  db_.PutEntity(1800, 1, WriteFlags::kCreateNew | WriteFlags::kExplicitDbid, make_blob("lease"),
                "lease", [&](PutResult value) { created = std::move(value); });
  ASSERT_TRUE(created.success) << created.error;
  CheckoutInfo original{Address(0x7F000001, 24991), 7, 9, 0, 111};
  GetResult acquired;
  db_.CheckoutEntity(1800, 1, original, [&](GetResult value) { acquired = std::move(value); });
  ASSERT_TRUE(acquired.success) << acquired.error;
  ASSERT_FALSE(acquired.checked_out_by);
  original.ownership_version = acquired.checkout_version;
  bool retired = false;
  db_.RetireCheckoutProcess(original.base_addr, original.process_instance, [&](Result<int> value) {
    retired = true;
    ASSERT_TRUE(value) << value.Error().Message();
    EXPECT_EQ(*value, 1);
  });
  ASSERT_TRUE(retired);
  DatabaseCheckoutOperation operation;
  operation.id[0] = std::byte{0x3A};
  operation.dbid = 1800;
  operation.type_id = 1;
  operation.owner = original;
  operation.owner.ownership_version = 0;
  GetResult rejected;
  db_.CheckoutWithOperation(operation, [&](GetResult value) { rejected = std::move(value); });
  EXPECT_FALSE(rejected.success);
  EXPECT_EQ(rejected.error_kind, DatabaseErrorKind::kOwnershipConflict);
  operation.id[0] = std::byte{0x3B};
  operation.owner.process_instance = 222;
  db_.CheckoutWithOperation(operation, [&](GetResult value) { acquired = std::move(value); });
  ASSERT_TRUE(acquired.success) << acquired.error;
  ASSERT_FALSE(acquired.checked_out_by);
  EXPECT_GT(acquired.checkout_version, original.ownership_version);
  db_.RetireCheckoutProcess(original.base_addr, original.process_instance, [](Result<int> value) {
    ASSERT_TRUE(value);
    EXPECT_EQ(*value, 0);
  });
  db_.Shutdown();
  ASSERT_TRUE(db_.Startup(cfg_, EntityDefRegistry::Instance()));
  GetResult current;
  db_.GetEntity(1800, 1, [&](GetResult value) { current = std::move(value); });
  ASSERT_TRUE(current.success);
  ASSERT_TRUE(current.checked_out_by);
  EXPECT_EQ(current.checked_out_by->process_instance, 222u);
  operation.id[0] = std::byte{0x3C};
  operation.owner.process_instance = 111;
  db_.CheckoutWithOperation(operation, [&](GetResult value) { rejected = std::move(value); });
  EXPECT_FALSE(rejected.success);
  EXPECT_EQ(rejected.error_kind, DatabaseErrorKind::kOwnershipConflict);
}

TEST_F(MysqlDatabaseTest, RangeRecoveryRetainsDeletedReceiptAndCheckoutIdsAfterRestart) {
  DatabaseWriteOperation operation;
  operation.id[0] = std::byte{0x5A};
  operation.requested_dbid = 1800;
  operation.requested_flags = WriteFlags::kCreateNew | WriteFlags::kExplicitDbid;
  PutResult created;
  db_.PutEntityWithOperation(operation, 1800, 1, operation.requested_flags, make_blob("receipt"),
                             "receipt", [&](PutResult result) { created = std::move(result); });
  ASSERT_TRUE(created.success) << created.error;
  DelResult deleted;
  db_.DelEntity(1800, 1, [&](DelResult result) { deleted = std::move(result); });
  ASSERT_TRUE(deleted.success);
  db_.GetMaxDbidInRange(1000, 2000, [](DbidRangeResult result) {
    ASSERT_TRUE(result.success) << result.error;
    EXPECT_EQ(result.max_dbid, 1800);
  });
  db_.PutEntity(1900, 1, WriteFlags::kCreateNew | WriteFlags::kExplicitDbid, make_blob("lease"),
                "lease", [&](PutResult result) { created = std::move(result); });
  ASSERT_TRUE(created.success) << created.error;
  CheckoutInfo owner{Address(0x7F000001, 24991), 7, 9};
  GetResult acquired;
  db_.CheckoutEntity(1900, 1, owner, [&](GetResult result) { acquired = std::move(result); });
  ASSERT_TRUE(acquired.success) << acquired.error;
  owner.ownership_version = acquired.checkout_version;
  db_.ReleaseCheckout(1900, 1, owner, [](Result<void> result) { ASSERT_TRUE(result); });
  db_.DelEntity(1900, 1, [&](DelResult result) { deleted = std::move(result); });
  ASSERT_TRUE(deleted.success);
  db_.Shutdown();
  ASSERT_TRUE(db_.Startup(cfg_, EntityDefRegistry::Instance()));
  db_.GetMaxDbidInRange(1000, 2000, [](DbidRangeResult result) {
    ASSERT_TRUE(result.success) << result.error;
    EXPECT_EQ(result.max_dbid, 1900);
  });
  db_.GetMaxDbidInRange(1800, 1900, [](DbidRangeResult result) {
    ASSERT_TRUE(result.success) << result.error;
    EXPECT_EQ(result.max_dbid, 1800);
  });
  db_.GetMaxDbidInRange(1901, 2000, [](DbidRangeResult result) {
    ASSERT_TRUE(result.success) << result.error;
    EXPECT_EQ(result.max_dbid, kInvalidDBID);
  });
}

TEST_F(MysqlDatabaseTest, GetMaxDbidInRangeReturnsExistingMaximum) {
  PutResult low;
  db_.PutEntity(1500, 1, WriteFlags::kCreateNew | WriteFlags::kExplicitDbid, make_blob("low"),
                "low", [&](PutResult r) { low = std::move(r); });
  ASSERT_TRUE(low.success) << low.error;

  PutResult high;
  db_.PutEntity(1750, 1, WriteFlags::kCreateNew | WriteFlags::kExplicitDbid, make_blob("high"),
                "high", [&](PutResult r) { high = std::move(r); });
  ASSERT_TRUE(high.success) << high.error;

  PutResult outside;
  db_.PutEntity(2500, 1, WriteFlags::kCreateNew | WriteFlags::kExplicitDbid, make_blob("outside"),
                "outside", [&](PutResult r) { outside = std::move(r); });
  ASSERT_TRUE(outside.success) << outside.error;

  DbidRangeResult range;
  db_.GetMaxDbidInRange(1000, 2000, [&](DbidRangeResult r) { range = std::move(r); });
  ASSERT_TRUE(range.success) << range.error;
  EXPECT_EQ(range.max_dbid, 1750);

  DbidRangeResult empty;
  db_.GetMaxDbidInRange(3000, 4000, [&](DbidRangeResult r) { empty = std::move(r); });
  ASSERT_TRUE(empty.success) << empty.error;
  EXPECT_EQ(empty.max_dbid, kInvalidDBID);
}

TEST_F(MysqlDatabaseTest, CheckoutAndClear) {
  PutResult put;
  db_.PutEntity(kInvalidDBID, 1, WriteFlags::kCreateNew, make_blob("data"), "bob",
                [&](PutResult r) { put = std::move(r); });
  ASSERT_TRUE(put.success) << put.error;

  CheckoutInfo owner;
  owner.base_addr = Address(0x7F000001, 7100);
  owner.app_id = 10;
  owner.entity_id = 20;

  GetResult checkout;
  db_.CheckoutEntity(put.dbid, 1, owner, [&](GetResult r) { checkout = std::move(r); });
  ASSERT_TRUE(checkout.success);
  EXPECT_FALSE(checkout.checked_out_by.has_value());

  GetResult second;
  db_.CheckoutEntity(put.dbid, 1, owner, [&](GetResult r) { second = std::move(r); });
  ASSERT_TRUE(second.success);
  ASSERT_TRUE(second.checked_out_by.has_value());
  EXPECT_EQ(second.checked_out_by->app_id, 10u);

  bool cleared = false;
  db_.ClearCheckout(put.dbid, 1, [&](Result<bool> ok) {
    ASSERT_TRUE(ok);
    cleared = *ok;
  });
  EXPECT_TRUE(cleared);
}

TEST_F(MysqlDatabaseTest, AutoLoadAndDeferredCallbacks) {
  db_.SetDeferredMode(true);

  PutResult put;
  bool put_done = false;
  db_.PutEntity(kInvalidDBID, 1, WriteFlags::kCreateNew | WriteFlags::kAutoLoadOn,
                make_blob("auto"), "carol", [&](PutResult r) {
                  put = std::move(r);
                  put_done = true;
                });

  EXPECT_FALSE(put.success);  // delivered only through ProcessResults
  pump_until([&] { return put_done; });
  ASSERT_TRUE(put.success) << put.error;

  std::vector<EntityData> entities;
  bool load_done = false;
  db_.GetAutoLoadEntities([&](Result<std::vector<EntityData>> rows) {
    ASSERT_TRUE(rows);
    entities = std::move(*rows);
    load_done = true;
  });
  EXPECT_TRUE(entities.empty());
  pump_until([&] { return load_done; });
  ASSERT_EQ(entities.size(), 1u);
  EXPECT_EQ(entities[0].identifier, "carol");
}

TEST_F(MysqlDatabaseTest, LookupByNameReturnsStoredPasswordHash) {
  PutResult put;
  db_.PutEntityWithPassword(kInvalidDBID, 1, WriteFlags::kCreateNew, make_blob("acct"), "diana",
                            "pw_hash_123", [&](PutResult r) { put = std::move(r); });
  ASSERT_TRUE(put.success) << put.error;

  LookupResult lookup;
  db_.LookupByName(1, "diana", [&](LookupResult r) { lookup = std::move(r); });
  ASSERT_TRUE(lookup.found);
  EXPECT_EQ(lookup.dbid, put.dbid);
  EXPECT_EQ(lookup.password_hash, "pw_hash_123");
}

TEST_F(MysqlDatabaseTest, PutEntityWithPasswordUsesExplicitDbid) {
  constexpr DatabaseID kDbid = 6000;

  PutResult put;
  db_.PutEntityWithPassword(kDbid, 1, WriteFlags::kCreateNew | WriteFlags::kExplicitDbid,
                            make_blob("acct"), "eve", "pw_hash_456",
                            [&](PutResult r) { put = std::move(r); });

  ASSERT_TRUE(put.success) << put.error;
  EXPECT_EQ(put.dbid, kDbid);

  LookupResult lookup;
  db_.LookupByName(1, "eve", [&](LookupResult r) { lookup = std::move(r); });
  ASSERT_TRUE(lookup.found);
  EXPECT_EQ(lookup.dbid, kDbid);
  EXPECT_EQ(lookup.password_hash, "pw_hash_456");
}

TEST_F(MysqlDatabaseTest, CheckoutByNameAndClearCheckoutsForAddress) {
  PutResult alice;
  PutResult bob;
  PutResult chris;
  db_.PutEntity(kInvalidDBID, 1, WriteFlags::kCreateNew, make_blob("a"), "alice",
                [&](PutResult r) { alice = std::move(r); });
  db_.PutEntity(kInvalidDBID, 1, WriteFlags::kCreateNew, make_blob("b"), "bob",
                [&](PutResult r) { bob = std::move(r); });
  db_.PutEntity(kInvalidDBID, 1, WriteFlags::kCreateNew, make_blob("c"), "chris",
                [&](PutResult r) { chris = std::move(r); });
  ASSERT_TRUE(alice.success);
  ASSERT_TRUE(bob.success);
  ASSERT_TRUE(chris.success);

  CheckoutInfo owner_a;
  owner_a.base_addr = Address(0x7F000001, 7201);
  owner_a.app_id = 21;
  owner_a.entity_id = 101;

  CheckoutInfo owner_b;
  owner_b.base_addr = Address(0x7F000001, 7202);
  owner_b.app_id = 22;
  owner_b.entity_id = 102;

  GetResult checkout_alice;
  db_.CheckoutEntityByName(1, "alice", owner_a,
                           [&](GetResult r) { checkout_alice = std::move(r); });
  ASSERT_TRUE(checkout_alice.success);
  EXPECT_FALSE(checkout_alice.checked_out_by.has_value());

  GetResult checkout_bob;
  db_.CheckoutEntity(bob.dbid, 1, owner_a, [&](GetResult r) { checkout_bob = std::move(r); });
  ASSERT_TRUE(checkout_bob.success);
  EXPECT_FALSE(checkout_bob.checked_out_by.has_value());

  GetResult checkout_chris;
  db_.CheckoutEntity(chris.dbid, 1, owner_b, [&](GetResult r) { checkout_chris = std::move(r); });
  ASSERT_TRUE(checkout_chris.success);
  EXPECT_FALSE(checkout_chris.checked_out_by.has_value());

  int cleared = 0;
  db_.ClearCheckoutsForAddress(owner_a.base_addr, [&](Result<int> count) {
    ASSERT_TRUE(count);
    cleared = *count;
  });
  EXPECT_EQ(cleared, 2);

  GetResult alice_after_clear;
  db_.GetEntity(alice.dbid, 1, [&](GetResult r) { alice_after_clear = std::move(r); });
  ASSERT_TRUE(alice_after_clear.success);
  EXPECT_FALSE(alice_after_clear.checked_out_by.has_value());

  GetResult chris_after_clear;
  db_.GetEntity(chris.dbid, 1, [&](GetResult r) { chris_after_clear = std::move(r); });
  ASSERT_TRUE(chris_after_clear.success);
  ASSERT_TRUE(chris_after_clear.checked_out_by.has_value());
  EXPECT_EQ(chris_after_clear.checked_out_by->app_id, owner_b.app_id);
}

TEST_F(MysqlDatabaseTest, PutEntityWithPasswordPreservesAutoLoadAndClearsCheckoutOnLogoff) {
  PutResult put;
  db_.PutEntityWithPassword(kInvalidDBID, 1, WriteFlags::kCreateNew | WriteFlags::kAutoLoadOn,
                            make_blob("acct"), "eve", "pw_hash_1",
                            [&](PutResult r) { put = std::move(r); });
  ASSERT_TRUE(put.success) << put.error;

  CheckoutInfo owner;
  owner.base_addr = Address(0x7F000001, 7301);
  owner.app_id = 31;
  owner.entity_id = 301;

  GetResult checkout;
  db_.CheckoutEntity(put.dbid, 1, owner, [&](GetResult r) { checkout = std::move(r); });
  ASSERT_TRUE(checkout.success);
  EXPECT_FALSE(checkout.checked_out_by.has_value());

  PutResult update;
  db_.PutEntityWithPassword(put.dbid, 1, WriteFlags::kLogOff, make_blob("acct_v2"), "", "pw_hash_2",
                            [&](PutResult r) { update = std::move(r); });
  ASSERT_TRUE(update.success) << update.error;
  EXPECT_EQ(update.dbid, put.dbid);

  GetResult get;
  db_.GetEntity(put.dbid, 1, [&](GetResult r) { get = std::move(r); });
  ASSERT_TRUE(get.success);
  EXPECT_EQ(get.data.blob, make_blob("acct_v2"));
  EXPECT_EQ(get.data.identifier, "eve");
  EXPECT_FALSE(get.checked_out_by.has_value());

  LookupResult lookup;
  db_.LookupByName(1, "eve", [&](LookupResult r) { lookup = std::move(r); });
  ASSERT_TRUE(lookup.found);
  EXPECT_EQ(lookup.dbid, put.dbid);
  EXPECT_EQ(lookup.password_hash, "pw_hash_2");

  std::vector<EntityData> auto_load_entities;
  db_.GetAutoLoadEntities([&](Result<std::vector<EntityData>> rows) {
    ASSERT_TRUE(rows);
    auto_load_entities = std::move(*rows);
  });
  ASSERT_EQ(auto_load_entities.size(), 1u);
  EXPECT_EQ(auto_load_entities[0].dbid, put.dbid);
}

TEST_F(MysqlDatabaseTest, SharedDatabaseReservationsAndShutdownNeverOverlap) {
  std::vector<EntityIdRange> ranges;
  MysqlDatabase peer;
  peer.SetDeferredMode(true);
  ASSERT_TRUE(peer.Startup(cfg_, EntityDefRegistry::Instance()).HasValue());
  db_.SetDeferredMode(true);
  auto collect = [&](Result<EntityIdRange> range) {
    EXPECT_TRUE(range.HasValue());
    if (range) ranges.push_back(*range);
  };
  for (int i = 0; i < 32; ++i) {
    db_.ReserveEntityIds(11, collect);
    peer.ReserveEntityIds(7, collect);
  }
  db_.Shutdown();
  peer.Shutdown();
  ASSERT_EQ(ranges.size(), 64u);
  std::sort(ranges.begin(), ranges.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
  EXPECT_EQ(ranges.front().first, 1u);
  for (size_t i = 1; i < ranges.size(); ++i) EXPECT_LT(ranges[i - 1].last, ranges[i].first);
  EXPECT_EQ(ranges.back().last, 32u * (11u + 7u));
  ASSERT_TRUE(db_.Startup(cfg_, EntityDefRegistry::Instance()).HasValue());
  db_.SetDeferredMode(false);
  db_.ReserveEntityIds(1, [](Result<EntityIdRange> range) {
    ASSERT_TRUE(range.HasValue());
    EXPECT_EQ(range->first, 32u * (11u + 7u) + 1);
  });
}

TEST_F(MysqlDatabaseTest, EntityIdExhaustionAndInvalidCountAreErrors) {
  db_.ReserveEntityIds(0, [](Result<EntityIdRange> range) { EXPECT_FALSE(range.HasValue()); });
  db_.ReserveEntityIds(kFirstLocalEntityID - 1, [](Result<EntityIdRange> range) {
    ASSERT_TRUE(range.HasValue());
    EXPECT_EQ(range->last, kFirstLocalEntityID - 1);
  });
  db_.ReserveEntityIds(1, [](Result<EntityIdRange> range) { EXPECT_FALSE(range.HasValue()); });
}

TEST_F(MysqlDatabaseTest, LostTransactionCannotReconnectAndPretendToCommit) {
  PutResult created;
  db_.PutEntity(0, 1, WriteFlags::kCreateNew, make_blob("committed"), "transaction",
                [&](PutResult result) { created = std::move(result); });
  ASSERT_TRUE(created.success);
  ASSERT_TRUE(MysqlDatabaseTestAccess::Begin(db_).HasValue());
  PutResult updated;
  db_.PutEntity(created.dbid, 1, WriteFlags::kNone, make_blob("uncommitted"), "transaction",
                [&](PutResult result) { updated = std::move(result); });
  ASSERT_TRUE(updated.success);
  auto admin = connect_admin(cfg_);
  ASSERT_NE(admin, nullptr);
  const auto kill = "KILL CONNECTION " + std::to_string(MysqlDatabaseTestAccess::ConnectionId(db_));
  ASSERT_EQ(mysql_real_query(admin.get(), kill.data(), static_cast<unsigned long>(kill.size())), 0);
  auto commit = MysqlDatabaseTestAccess::Commit(db_);
  ASSERT_FALSE(commit.HasValue());
  EXPECT_EQ(commit.Error().Code(), ErrorCode::kCommitUnknown);
  GetResult loaded;
  db_.GetEntity(created.dbid, 1, [&](GetResult result) { loaded = std::move(result); });
  ASSERT_TRUE(loaded.success);
  EXPECT_EQ(loaded.data.blob, make_blob("committed"));
}

TEST_F(MysqlDatabaseTest, LostCommitReplyKeepsReceiptAndCannotRepeatCreate) {
  auto upstream = Address::Resolve(cfg_.mysql_host, cfg_.mysql_port);
  ASSERT_TRUE(upstream);
  auto listener = Socket::CreateTcp();
  ASSERT_TRUE(listener);
  ASSERT_TRUE(listener->Bind(Address("127.0.0.1", 0)));
  ASSERT_TRUE(listener->Listen());
  auto local = listener->LocalAddress();
  ASSERT_TRUE(local);
  std::atomic<bool> armed{false};
  std::atomic<bool> dropped{false};
  std::string proxy_error;
  std::jthread proxy([&, socket = std::move(*listener)](std::stop_token stop) mutable {
    auto result =
        RelayUntilCommittedReply(std::move(socket), *upstream, cfg_, stop, armed, dropped);
    if (!result) proxy_error = result.Error().Message();
  });
  auto config = cfg_;
  config.mysql_host = "127.0.0.1";
  config.mysql_port = local->Port();
  MysqlDatabase peer;
  ASSERT_TRUE(peer.Startup(config, EntityDefRegistry::Instance()));
  EXPECT_EQ(MysqlDatabaseTestAccess::UsesTls(peer), MysqlDatabaseTestAccess::UsesTls(db_));
  RecordProperty("commit_reply_proxy_tls", MysqlDatabaseTestAccess::UsesTls(peer) ? 1 : 0);
  DatabaseWriteOperation create;
  create.id[0] = std::byte{0x7C};
  create.requested_flags = WriteFlags::kCreateNew;
  create.entity_id = 9001;
  create.create_owner = CheckoutInfo{Address("127.0.0.1", 24991), 7, 9001, 0, 111};
  armed.store(true);
  PutResult unknown;
  peer.PutEntityWithOperation(create, 0, 1, WriteFlags::kCreateNew, make_blob("saved"),
                              "lost_commit_reply", [&](PutResult r) { unknown = std::move(r); });
  peer.Shutdown();
  proxy.request_stop();
  proxy.join();
  ASSERT_TRUE(proxy_error.empty()) << proxy_error;
  ASSERT_TRUE(dropped.load());
  EXPECT_FALSE(unknown.success);
  EXPECT_EQ(unknown.error_kind, DatabaseErrorKind::kCommitUnknown);
  PutResult replay;
  db_.PutEntityWithOperation(create, 0, 1, WriteFlags::kCreateNew, make_blob("saved"),
                             "lost_commit_reply", [&](PutResult r) { replay = std::move(r); });
  ASSERT_TRUE(replay.success) << replay.error;
  ASSERT_TRUE(replay.checkout_owner);
  EXPECT_EQ(replay.checkout_owner->ownership_version, 1u);
  GetResult stored;
  db_.GetEntity(replay.dbid, 1, [&](GetResult r) { stored = std::move(r); });
  ASSERT_TRUE(stored.success);
  EXPECT_EQ(stored.data.blob, make_blob("saved"));
  EXPECT_EQ(stored.checked_out_by, replay.checkout_owner);
  db_.Shutdown();
  ASSERT_TRUE(db_.Startup(cfg_, EntityDefRegistry::Instance()));
  PutResult restarted;
  db_.PutEntityWithOperation(create, 0, 1, WriteFlags::kCreateNew, make_blob("saved"),
                             "lost_commit_reply", [&](PutResult r) { restarted = std::move(r); });
  ASSERT_TRUE(restarted.success) << restarted.error;
  EXPECT_EQ(restarted.dbid, replay.dbid);
  EXPECT_EQ(restarted.checkout_owner, replay.checkout_owner);
}

TEST_F(MysqlDatabaseTest, MissingEntityIdCounterIsNotAnInitialCounter) {
  db_.ReserveEntityIds(10, [](Result<EntityIdRange> range) { ASSERT_TRUE(range.HasValue()); });
  auto admin = connect_admin(cfg_);
  ASSERT_NE(admin, nullptr);
  constexpr std::string_view sql = "DELETE FROM atlas_entity_id_counter WHERE id = 1";
  ASSERT_EQ(mysql_real_query(admin.get(), sql.data(), static_cast<unsigned long>(sql.size())), 0);
  db_.ReserveEntityIds(10, [](Result<EntityIdRange> range) {
    ASSERT_FALSE(range.HasValue());
    EXPECT_EQ(range.Error().Code(), ErrorCode::kIoError);
  });
  db_.Shutdown();
  ASSERT_TRUE(db_.Startup(cfg_, EntityDefRegistry::Instance()).HasValue());
  db_.ReserveEntityIds(1, [](Result<EntityIdRange> range) { EXPECT_FALSE(range.HasValue()); });
}

TEST_F(MysqlDatabaseTest, MissingEntityHasExplicitErrorKind) {
  GetResult result;
  db_.GetEntity(987654, 1, [&](GetResult r) { result = std::move(r); });
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.error_kind, DatabaseErrorKind::kNotFound);
  db_.CheckoutEntity(987654, 1, CheckoutInfo{}, [&](GetResult r) { result = std::move(r); });
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.error_kind, DatabaseErrorKind::kNotFound);
  db_.CheckoutEntityByName(1, "missing_entity", CheckoutInfo{},
                           [&](GetResult r) { result = std::move(r); });
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.error_kind, DatabaseErrorKind::kNotFound);
}

TEST_F(MysqlDatabaseTest, ReplayedCreateReturnsOriginalDbidAcrossBackendsAndRestart) {
  DatabaseWriteOperation operation;
  operation.id[0] = std::byte{1};
  operation.requested_flags = WriteFlags::kCreateNew;
  PutResult first;
  db_.PutEntityWithOperation(operation, 0, 1, WriteFlags::kCreateNew, make_blob("original"), "",
                             [&](PutResult result) { first = std::move(result); });
  ASSERT_TRUE(first.success);
  MysqlDatabase peer;
  ASSERT_TRUE(peer.Startup(cfg_, EntityDefRegistry::Instance()).HasValue());
  PutResult replay;
  peer.PutEntityWithOperation(operation, 0, 1, WriteFlags::kCreateNew, make_blob("original"), "",
                              [&](PutResult result) { replay = std::move(result); });
  ASSERT_TRUE(replay.success);
  EXPECT_EQ(replay.dbid, first.dbid);
  peer.Shutdown();
  db_.Shutdown();
  ASSERT_TRUE(db_.Startup(cfg_, EntityDefRegistry::Instance()).HasValue());
  db_.PutEntityWithOperation(operation, 0, 1, WriteFlags::kCreateNew, make_blob("original"), "",
                             [&](PutResult result) { replay = std::move(result); });
  ASSERT_TRUE(replay.success);
  EXPECT_EQ(replay.dbid, first.dbid);
}

TEST_F(MysqlDatabaseTest, ReusedOperationIdWithChangedPayloadIsRejected) {
  DatabaseWriteOperation operation;
  operation.id[0] = std::byte{1};
  operation.requested_flags = WriteFlags::kCreateNew;
  PutResult result;
  db_.PutEntityWithOperation(operation, 0, 1, WriteFlags::kCreateNew, make_blob("original"), "",
                             [&](PutResult r) { result = std::move(r); });
  ASSERT_TRUE(result.success);
  db_.PutEntityWithOperation(operation, 0, 1, WriteFlags::kCreateNew, make_blob("changed"), "",
                             [&](PutResult r) { result = std::move(r); });
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.error_kind, DatabaseErrorKind::kOperationConflict);
}

TEST_F(MysqlDatabaseTest, ResolvingUnknownCheckoutNeverAcquiresAnEntity) {
  db_.PutEntity(42, 1, WriteFlags::kCreateNew | WriteFlags::kExplicitDbid, {}, "owned",
                [](PutResult r) { ASSERT_TRUE(r.success); });
  DatabaseCheckoutOperation operation;
  operation.id[0] = std::byte{91};
  operation.dbid = 42;
  operation.type_id = 1;
  operation.owner = CheckoutInfo{Address(0x7F000001, 7100), 3, 77};
  GetResult result;
  db_.ResolveCheckoutOperation(operation, [&](GetResult r) { result = std::move(r); });
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.error_kind, DatabaseErrorKind::kCommitUnknown);
  db_.GetEntity(42, 1, [](GetResult r) {
    ASSERT_TRUE(r.success);
    EXPECT_FALSE(r.checked_out_by);
  });
  db_.CheckoutWithOperation(operation, [&](GetResult r) { result = std::move(r); });
  ASSERT_TRUE(result.success) << result.error;
  const auto version = result.checkout_version;
  ASSERT_EQ(version, 1u);
  db_.ResolveCheckoutOperation(operation, [&](GetResult r) { result = std::move(r); });
  ASSERT_TRUE(result.success) << result.error;
  EXPECT_EQ(result.checkout_version, version);
  auto owner = operation.owner;
  owner.ownership_version = version;
  db_.ReleaseCheckout(42, 1, owner, [](Result<void> r) { ASSERT_TRUE(r); });
  db_.ResolveCheckoutOperation(operation, [&](GetResult r) { result = std::move(r); });
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.error_kind, DatabaseErrorKind::kOwnershipConflict);
  EXPECT_EQ(result.data.dbid, 42);
  EXPECT_EQ(result.checkout_version, version);
  db_.GetEntity(42, 1, [](GetResult r) {
    ASSERT_TRUE(r.success);
    EXPECT_FALSE(r.checked_out_by);
  });
}

TEST_F(MysqlDatabaseTest, NamedCheckoutRetryBindsIdentityAndOperationKind) {
  PutResult created;
  db_.PutEntity(42, 1, WriteFlags::kCreateNew | WriteFlags::kExplicitDbid, make_blob("original"),
                "original_name", [&](PutResult r) { created = std::move(r); });
  ASSERT_TRUE(created.success);
  DatabaseWriteOperation write;
  write.id[0] = std::byte{93};
  write.requested_dbid = 42;
  db_.PutEntityWithOperation(write, 42, 1, WriteFlags::kNone, make_blob("original"),
                             "original_name",
                             [](PutResult r) { ASSERT_TRUE(r.success) << r.error; });
  DatabaseCheckoutOperation operation;
  operation.id = write.id;
  operation.dbid = 42;
  operation.type_id = 1;
  operation.owner = CheckoutInfo{Address(0x7F000001, 7100), 3, 77};
  GetResult result;
  db_.CheckoutWithOperation(operation, [&](GetResult r) { result = std::move(r); });
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.error_kind, DatabaseErrorKind::kOperationConflict);
  operation.id[0] = std::byte{92};
  operation.dbid = kInvalidDBID;
  operation.identifier = "original_name";
  std::vector<DatabaseCheckoutOperation> invalid(6, operation);
  invalid[0].id = {};
  invalid[1].type_id = 0;
  invalid[2].dbid = -1;
  invalid[3].owner.ownership_version = 1;
  invalid[4].owner.base_addr = Address{};
  invalid[5].identifier.clear();
  for (const auto& input : invalid) {
    db_.CheckoutWithOperation(input, [&](GetResult r) { result = std::move(r); });
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.error_kind, DatabaseErrorKind::kOperationConflict);
  }
  db_.CheckoutWithOperation(operation, [&](GetResult r) { result = std::move(r); });
  ASSERT_TRUE(result.success) << result.error;
  ASSERT_FALSE(result.checked_out_by);
  ASSERT_EQ(result.data.dbid, 42);
  const auto version = result.checkout_version;
  ASSERT_GT(version, 0u);
  db_.PutEntity(42, 1, WriteFlags::kNone, make_blob("original"), "renamed",
                [](PutResult r) { ASSERT_TRUE(r.success) << r.error; });
  db_.CheckoutWithOperation(operation, [&](GetResult r) { result = std::move(r); });
  ASSERT_TRUE(result.success) << result.error;
  EXPECT_EQ(result.data.dbid, 42);
  EXPECT_EQ(result.checkout_version, version);
  auto changed = operation;
  changed.identifier = "renamed";
  db_.CheckoutWithOperation(changed, [&](GetResult r) { result = std::move(r); });
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.error_kind, DatabaseErrorKind::kOperationConflict);
  changed = operation;
  changed.id[0] = std::byte{94};
  db_.CheckoutWithOperation(changed, [&](GetResult r) { result = std::move(r); });
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.error_kind, DatabaseErrorKind::kNotFound);
  changed.identifier = "renamed";
  db_.CheckoutWithOperation(changed, [&](GetResult r) { result = std::move(r); });
  ASSERT_TRUE(result.success) << result.error;
  ASSERT_TRUE(result.checked_out_by);
  EXPECT_EQ(result.checked_out_by->ownership_version, version);
}

TEST_F(MysqlDatabaseTest, CheckoutRetryKeepsOriginalLeaseAfterRestart) {
  PutResult created;
  db_.PutEntity(42, 1, WriteFlags::kCreateNew | WriteFlags::kExplicitDbid, make_blob("original"),
                "durable_checkout", [&](PutResult r) { created = std::move(r); });
  ASSERT_TRUE(created.success);
  DatabaseCheckoutOperation operation;
  operation.id[0] = std::byte{91};
  operation.dbid = 42;
  operation.type_id = 1;
  operation.owner = CheckoutInfo{Address(0x7F000001, 7100), 3, 77};
  GetResult acquired;
  db_.CheckoutWithOperation(operation, [&](GetResult r) { acquired = std::move(r); });
  ASSERT_TRUE(acquired.success) << acquired.error;
  ASSERT_GT(acquired.checkout_version, 0u);
  EXPECT_FALSE(acquired.checked_out_by);
  db_.Shutdown();
  DatabaseConfig config = cfg_;
  ASSERT_TRUE(db_.Startup(config, EntityDefRegistry::Instance()));
  GetResult replay;
  db_.CheckoutWithOperation(operation, [&](GetResult r) { replay = std::move(r); });
  ASSERT_TRUE(replay.success) << replay.error;
  EXPECT_EQ(replay.checkout_version, acquired.checkout_version);
  EXPECT_EQ(replay.data.blob, make_blob("original"));
  EXPECT_FALSE(replay.checked_out_by);
  auto changed = operation;
  changed.owner.entity_id++;
  db_.CheckoutWithOperation(changed, [&](GetResult r) { replay = std::move(r); });
  EXPECT_FALSE(replay.success);
  EXPECT_EQ(replay.error_kind, DatabaseErrorKind::kOperationConflict);
  auto held = operation.owner;
  held.ownership_version = acquired.checkout_version;
  db_.ReleaseCheckout(42, 1, held, [](Result<void> r) { EXPECT_TRUE(r); });
  GetResult next;
  db_.CheckoutEntity(42, 1, changed.owner, [&](GetResult r) { next = std::move(r); });
  ASSERT_TRUE(next.success);
  EXPECT_GT(next.checkout_version, acquired.checkout_version);
  db_.CheckoutWithOperation(operation, [&](GetResult r) { replay = std::move(r); });
  EXPECT_FALSE(replay.success);
  EXPECT_EQ(replay.error_kind, DatabaseErrorKind::kOwnershipConflict);
  auto newer = changed.owner;
  newer.ownership_version = next.checkout_version;
  db_.ReleaseCheckout(42, 1, newer, [](Result<void> r) { EXPECT_TRUE(r); });
  db_.CheckoutWithOperation(operation, [&](GetResult r) { replay = std::move(r); });
  EXPECT_FALSE(replay.success);
  EXPECT_EQ(replay.error_kind, DatabaseErrorKind::kOwnershipConflict);
  db_.Shutdown();
  db_.CheckoutWithOperation(operation, [&](GetResult r) { replay = std::move(r); });
  EXPECT_FALSE(replay.success);
  EXPECT_FALSE(replay.error.empty());
  EXPECT_NE(replay.error_kind, DatabaseErrorKind::kNotFound);
}

TEST_F(MysqlDatabaseTest, ConcurrentReplayCreatesOnlyOneEntity) {
  MysqlDatabase peer;
  ASSERT_TRUE(peer.Startup(cfg_, EntityDefRegistry::Instance()).HasValue());
  db_.SetDeferredMode(true);
  peer.SetDeferredMode(true);
  DatabaseWriteOperation operation;
  operation.id[0] = std::byte{9};
  operation.requested_flags = WriteFlags::kCreateNew;
  operation.entity_id = 77;
  operation.create_owner = CheckoutInfo{Address(0x7F000001, 7100), 3, 77};
  std::vector<PutResult> results;
  for (int i = 0; i < 16; ++i) {
    auto& target = i % 2 == 0 ? db_ : peer;
    target.PutEntityWithOperation(operation, 0, 1, WriteFlags::kCreateNew, make_blob("same"), "",
                                  [&](PutResult result) { results.push_back(std::move(result)); });
  }
  db_.Shutdown();
  peer.Shutdown();
  ASSERT_EQ(results.size(), 16u);
  for (const auto& result : results) {
    EXPECT_TRUE(result.success) << result.error;
    EXPECT_EQ(result.dbid, results.front().dbid);
    ASSERT_TRUE(result.checkout_owner.has_value());
    EXPECT_EQ(result.checkout_owner->ownership_version, 1u);
  }
  auto admin = connect_admin(cfg_);
  ASSERT_NE(admin, nullptr);
  ASSERT_EQ(mysql_query(admin.get(), "SELECT COUNT(*) FROM entities"), 0);
  std::unique_ptr<MYSQL_RES, decltype(&mysql_free_result)> rows(mysql_store_result(admin.get()),
                                                                mysql_free_result);
  ASSERT_NE(rows, nullptr);
  auto row = mysql_fetch_row(rows.get());
  ASSERT_NE(row, nullptr);
  EXPECT_STREQ(row[0], "1");
}

TEST_F(MysqlDatabaseTest, ConcurrentAccountCreatesKeepOneIdentityAndCredentials) {
  MysqlDatabase peer;
  ASSERT_TRUE(peer.Startup(cfg_, EntityDefRegistry::Instance()));
  db_.SetDeferredMode(true);
  peer.SetDeferredMode(true);
  std::vector<PutResult> results;
  std::string created_hash;
  for (int i = 0; i < 16; ++i) {
    auto& target = i % 2 == 0 ? db_ : peer;
    const std::string password_hash = i % 2 == 0 ? "first_hash" : "second_hash";
    target.PutEntityWithPassword(0, 1, WriteFlags::kCreateNew, make_blob("account"),
                                 "racing_account", password_hash, [&, password_hash](PutResult r) {
                                   if (r.success) created_hash = password_hash;
                                   results.push_back(std::move(r));
                                 });
  }
  db_.Shutdown();
  peer.Shutdown();
  ASSERT_EQ(results.size(), 16u);
  DatabaseID created = kInvalidDBID;
  for (const auto& result : results) {
    if (result.success) {
      EXPECT_EQ(created, kInvalidDBID);
      created = result.dbid;
    } else {
      EXPECT_EQ(result.error_kind, DatabaseErrorKind::kDuplicateIdentifier) << result.error;
    }
  }
  ASSERT_NE(created, kInvalidDBID);
  ASSERT_TRUE(db_.Startup(cfg_, EntityDefRegistry::Instance()));
  db_.SetDeferredMode(false);
  LookupResult account;
  db_.LookupByName(1, "racing_account", [&](LookupResult r) { account = std::move(r); });
  ASSERT_TRUE(account.found);
  EXPECT_EQ(account.dbid, created);
  EXPECT_EQ(account.password_hash, created_hash);
  auto admin = connect_admin(cfg_);
  ASSERT_NE(admin, nullptr);
  ASSERT_EQ(mysql_query(admin.get(), "SELECT COUNT(*) FROM entities"), 0);
  std::unique_ptr<MYSQL_RES, decltype(&mysql_free_result)> rows(mysql_store_result(admin.get()),
                                                                mysql_free_result);
  ASSERT_NE(rows, nullptr);
  auto row = mysql_fetch_row(rows.get());
  ASSERT_NE(row, nullptr);
  EXPECT_STREQ(row[0], "1");
}

TEST_F(MysqlDatabaseTest, ConcurrentCheckoutReplayAcrossConnectionsKeepsOneLease) {
  db_.PutEntity(42, 1, WriteFlags::kCreateNew | WriteFlags::kExplicitDbid, {}, "owned",
                [](PutResult r) { ASSERT_TRUE(r.success); });
  MysqlDatabase peer;
  ASSERT_TRUE(peer.Startup(cfg_, EntityDefRegistry::Instance()));
  db_.SetDeferredMode(true);
  peer.SetDeferredMode(true);
  DatabaseCheckoutOperation operation;
  operation.id[0] = std::byte{91};
  operation.dbid = 42;
  operation.type_id = 1;
  operation.owner = CheckoutInfo{Address(0x7F000001, 7100), 3, 77};
  std::vector<GetResult> replies;
  for (int i = 0; i < 16; ++i) {
    auto& target = i % 2 == 0 ? db_ : peer;
    target.CheckoutWithOperation(operation, [&](GetResult r) { replies.push_back(std::move(r)); });
  }
  db_.Shutdown();
  peer.Shutdown();
  ASSERT_EQ(replies.size(), 16u);
  for (const auto& reply : replies) {
    EXPECT_TRUE(reply.success) << reply.error;
    EXPECT_FALSE(reply.checked_out_by);
    EXPECT_EQ(reply.data.dbid, 42);
    EXPECT_EQ(reply.checkout_version, 1u);
  }
}

TEST_F(MysqlDatabaseTest, CheckoutReceiptFailureRollsBackLeaseAndVersion) {
  db_.PutEntity(42, 1, WriteFlags::kCreateNew | WriteFlags::kExplicitDbid, {}, "owned",
                [](PutResult r) { ASSERT_TRUE(r.success); });
  auto admin = connect_admin(cfg_);
  ASSERT_NE(admin, nullptr);
  ASSERT_EQ(mysql_query(admin.get(),
                        "CREATE TRIGGER reject_checkout_receipt BEFORE UPDATE "
                        "ON atlas_write_receipts FOR EACH ROW SIGNAL SQLSTATE '45000' "
                        "SET MESSAGE_TEXT = 'injected receipt failure'"),
            0);
  DatabaseCheckoutOperation operation;
  operation.id[0] = std::byte{91};
  operation.dbid = 42;
  operation.type_id = 1;
  operation.owner = CheckoutInfo{Address(0x7F000001, 7100), 3, 77};
  GetResult result;
  db_.CheckoutWithOperation(operation, [&](GetResult r) { result = std::move(r); });
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.checkout_version, 0u);
  db_.GetEntity(42, 1, [](GetResult r) {
    ASSERT_TRUE(r.success);
    EXPECT_FALSE(r.checked_out_by);
  });
  ASSERT_EQ(mysql_query(admin.get(), "DROP TRIGGER reject_checkout_receipt"), 0);
  db_.CheckoutWithOperation(operation, [&](GetResult r) { result = std::move(r); });
  ASSERT_TRUE(result.success) << result.error;
  EXPECT_EQ(result.checkout_version, 1u);
}

TEST_F(MysqlDatabaseTest, IncompleteCheckoutReceiptCannotBeReexecuted) {
  db_.PutEntity(42, 1, WriteFlags::kCreateNew | WriteFlags::kExplicitDbid, {}, "owned",
                [](PutResult r) { ASSERT_TRUE(r.success); });
  DatabaseCheckoutOperation operation;
  operation.id[0] = std::byte{91};
  operation.dbid = 42;
  operation.type_id = 1;
  operation.owner = CheckoutInfo{Address(0x7F000001, 7100), 3, 77};
  GetResult result;
  db_.CheckoutWithOperation(operation, [&](GetResult r) { result = std::move(r); });
  ASSERT_TRUE(result.success) << result.error;
  auto owner = operation.owner;
  owner.ownership_version = result.checkout_version;
  auto admin = connect_admin(cfg_);
  ASSERT_NE(admin, nullptr);
  ASSERT_EQ(mysql_query(admin.get(),
                        "UPDATE atlas_write_receipts SET result_dbid = NULL, "
                        "result_checkout_version = 0"),
            0);
  db_.ReleaseCheckout(42, 1, owner, [](Result<void> r) { ASSERT_TRUE(r); });
  db_.CheckoutWithOperation(operation, [&](GetResult r) { result = std::move(r); });
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.error_kind, DatabaseErrorKind::kCommitUnknown);
  EXPECT_EQ(result.checkout_version, 0u);
  db_.GetEntity(42, 1, [](GetResult r) {
    ASSERT_TRUE(r.success);
    EXPECT_FALSE(r.checked_out_by);
  });
}

TEST_F(MysqlDatabaseTest, FailedReceiptWriteRollsBackEntityAndCanBeRetried) {
  auto admin = connect_admin(cfg_);
  ASSERT_NE(admin, nullptr);
  ASSERT_EQ(mysql_query(admin.get(),
                        "CREATE TRIGGER reject_receipt BEFORE UPDATE ON atlas_write_receipts "
                        "FOR EACH ROW SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'injected'"),
            0);
  DatabaseWriteOperation operation;
  operation.id[0] = std::byte{3};
  operation.requested_flags = WriteFlags::kCreateNew;
  PutResult result;
  db_.PutEntityWithOperation(operation, 0, 1, WriteFlags::kCreateNew, make_blob("data"), "unique",
                             [&](PutResult r) { result = std::move(r); });
  EXPECT_FALSE(result.success);
  LookupResult lookup;
  db_.LookupByName(1, "unique", [&](LookupResult r) { lookup = std::move(r); });
  EXPECT_FALSE(lookup.found);
  ASSERT_EQ(mysql_query(admin.get(), "DROP TRIGGER reject_receipt"), 0);
  db_.PutEntityWithOperation(operation, 0, 1, WriteFlags::kCreateNew, make_blob("data"), "unique",
                             [&](PutResult r) { result = std::move(r); });
  ASSERT_TRUE(result.success) << result.error;
}

TEST_F(MysqlDatabaseTest, ReceiptSurvivesDeleteAndRejectsMissingOperationId) {
  DatabaseWriteOperation operation;
  operation.requested_flags = WriteFlags::kCreateNew;
  PutResult created;
  db_.PutEntityWithOperation(operation, 0, 1, WriteFlags::kCreateNew, make_blob("data"), "",
                             [&](PutResult r) { created = std::move(r); });
  EXPECT_FALSE(created.success);
  EXPECT_EQ(created.error_kind, DatabaseErrorKind::kOperationConflict);
  operation.id[0] = std::byte{5};
  db_.PutEntityWithOperation(operation, 0, 1, WriteFlags::kCreateNew, make_blob("data"), "",
                             [&](PutResult r) { created = std::move(r); });
  ASSERT_TRUE(created.success);
  DatabaseWriteOperation removal;
  removal.id[0] = std::byte{6};
  removal.requested_dbid = created.dbid;
  removal.requested_flags = WriteFlags::kDelete;
  PutResult removed;
  db_.PutEntityWithOperation(removal, created.dbid, 1, WriteFlags::kDelete, {}, "",
                             [&](PutResult r) { removed = std::move(r); });
  ASSERT_TRUE(removed.success);
  EXPECT_EQ(removed.dbid, created.dbid);
  db_.PutEntityWithOperation(removal, created.dbid, 1, WriteFlags::kDelete, {}, "",
                             [&](PutResult r) { removed = std::move(r); });
  EXPECT_TRUE(removed.success);
  PutResult replay;
  db_.PutEntityWithOperation(operation, 0, 1, WriteFlags::kCreateNew, make_blob("data"), "",
                             [&](PutResult r) { replay = std::move(r); });
  EXPECT_TRUE(replay.success);
  EXPECT_EQ(replay.dbid, created.dbid);
  GetResult missing;
  db_.GetEntity(created.dbid, 1, [&](GetResult r) { missing = std::move(r); });
  EXPECT_FALSE(missing.success);
}

TEST_F(MysqlDatabaseTest, ReplayedShardAllocatedCreateKeepsOriginalDbid) {
  DatabaseWriteOperation operation;
  operation.id[0] = std::byte{7};
  operation.requested_flags = WriteFlags::kCreateNew;
  PutResult result;
  const auto flags = WriteFlags::kCreateNew | WriteFlags::kExplicitDbid;
  db_.PutEntityWithOperation(operation, 500, 1, flags, make_blob("same"), "",
                             [&](PutResult r) { result = std::move(r); });
  ASSERT_TRUE(result.success);
  EXPECT_EQ(result.dbid, 500);
  db_.PutEntityWithOperation(operation, 900, 1, flags, make_blob("same"), "",
                             [&](PutResult r) { result = std::move(r); });
  ASSERT_TRUE(result.success);
  EXPECT_EQ(result.dbid, 500);
}

TEST_F(MysqlDatabaseTest, OwnershipVersionFencesStaleWritesAndReleaseAfterReacquire) {
  PutResult created;
  db_.PutEntity(0, 1, WriteFlags::kCreateNew, make_blob("saved"), "owned",
                [&](PutResult r) { created = std::move(r); });
  ASSERT_TRUE(created.success);
  CheckoutInfo owner{Address(0x7F000001, 7100), 3, 21};
  GetResult checkout;
  db_.CheckoutEntity(created.dbid, 1, owner, [&](GetResult r) { checkout = std::move(r); });
  ASSERT_TRUE(checkout.success);
  EXPECT_GT(checkout.checkout_version, 0u);
  owner.ownership_version = checkout.checkout_version;
  bool released = false;
  db_.ReleaseCheckout(created.dbid, 1, owner, [&](Result<void> r) { released = r.HasValue(); });
  ASSERT_TRUE(released);
  db_.CheckoutEntity(created.dbid, 1, owner, [&](GetResult r) { checkout = std::move(r); });
  ASSERT_TRUE(checkout.success);
  ASSERT_FALSE(checkout.checked_out_by.has_value());
  EXPECT_GT(checkout.checkout_version, owner.ownership_version);
  MysqlDatabase peer;
  ASSERT_TRUE(peer.Startup(cfg_, EntityDefRegistry::Instance()).HasValue());
  peer.ReleaseCheckout(created.dbid, 1, owner, [](Result<void> r) {
    ASSERT_FALSE(r.HasValue());
    EXPECT_EQ(r.Error().Code(), ErrorCode::kPermissionDenied);
  });
  for (auto flags : {WriteFlags::kNone, WriteFlags::kLogOff, WriteFlags::kDelete}) {
    DatabaseWriteOperation stale;
    stale.id[0] = static_cast<std::byte>(static_cast<uint8_t>(flags) + 1);
    stale.requested_dbid = created.dbid;
    stale.requested_flags = flags;
    stale.expected_owner = owner;
    peer.PutEntityWithOperation(stale, created.dbid, 1, flags, make_blob("stale"), "",
                                [](PutResult r) {
                                  EXPECT_FALSE(r.success);
                                  EXPECT_EQ(r.error_kind, DatabaseErrorKind::kOwnershipConflict);
                                });
  }
  GetResult loaded;
  peer.GetEntity(created.dbid, 1, [&](GetResult r) { loaded = std::move(r); });
  ASSERT_TRUE(loaded.success);
  EXPECT_EQ(loaded.data.blob, make_blob("saved"));
  ASSERT_TRUE(loaded.checked_out_by.has_value());
  EXPECT_EQ(loaded.checked_out_by->ownership_version, checkout.checkout_version);
  peer.Shutdown();
}

TEST_F(MysqlDatabaseTest, CurrentOwnerCanWriteAndLogoffButCannotChangeReceiptOwner) {
  PutResult put;
  db_.PutEntity(0, 1, WriteFlags::kCreateNew, make_blob("old"), "current",
                [&](PutResult r) { put = std::move(r); });
  ASSERT_TRUE(put.success);
  CheckoutInfo owner{Address(0x7F000001, 7100), 8, 12};
  GetResult checkout;
  db_.CheckoutEntityByName(1, "current", owner, [&](GetResult r) { checkout = std::move(r); });
  ASSERT_TRUE(checkout.success);
  ASSERT_GT(checkout.checkout_version, 0u);
  owner.ownership_version = checkout.checkout_version;
  DatabaseWriteOperation write;
  write.id[0] = std::byte{11};
  write.requested_dbid = put.dbid;
  write.requested_flags = WriteFlags::kLogOff;
  write.expected_owner = owner;
  PutResult result;
  db_.PutEntityWithOperation(write, put.dbid, 1, WriteFlags::kLogOff, make_blob("new"), "",
                             [&](PutResult r) { result = std::move(r); });
  ASSERT_TRUE(result.success) << result.error;
  db_.CheckoutEntity(put.dbid, 1, owner, [&](GetResult r) { checkout = std::move(r); });
  ASSERT_TRUE(checkout.success);
  ASSERT_FALSE(checkout.checked_out_by.has_value());
  EXPECT_GT(checkout.checkout_version, owner.ownership_version);
  db_.PutEntityWithOperation(write, put.dbid, 1, WriteFlags::kLogOff, make_blob("new"), "",
                             [&](PutResult r) { result = std::move(r); });
  ASSERT_TRUE(result.success);
  GetResult loaded;
  db_.GetEntity(put.dbid, 1, [&](GetResult r) { loaded = std::move(r); });
  ASSERT_TRUE(loaded.checked_out_by.has_value());
  EXPECT_EQ(loaded.checked_out_by->ownership_version, checkout.checkout_version);
  write.expected_owner->ownership_version = checkout.checkout_version;
  db_.PutEntityWithOperation(write, put.dbid, 1, WriteFlags::kLogOff, make_blob("new"), "",
                             [&](PutResult r) { result = std::move(r); });
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.error_kind, DatabaseErrorKind::kOperationConflict);
}

TEST_F(MysqlDatabaseTest, CheckoutVersionSurvivesRestartAndExplicitDbidReuse) {
  PutResult put;
  db_.PutEntity(500, 1, WriteFlags::kCreateNew | WriteFlags::kExplicitDbid, make_blob("one"), "",
                [&](PutResult r) { put = std::move(r); });
  ASSERT_TRUE(put.success);
  CheckoutInfo owner{Address(0x7F000001, 7100), 9, 15};
  GetResult checkout;
  db_.CheckoutEntity(500, 1, owner, [&](GetResult r) { checkout = std::move(r); });
  ASSERT_TRUE(checkout.success);
  owner.ownership_version = checkout.checkout_version;
  db_.DelEntity(500, 1, [](DelResult r) { ASSERT_TRUE(r.success); });
  db_.Shutdown();
  ASSERT_TRUE(db_.Startup(cfg_, EntityDefRegistry::Instance()).HasValue());
  db_.PutEntity(500, 1, WriteFlags::kCreateNew | WriteFlags::kExplicitDbid, make_blob("two"), "",
                [&](PutResult r) { put = std::move(r); });
  ASSERT_TRUE(put.success);
  db_.CheckoutEntity(500, 1, owner, [&](GetResult r) { checkout = std::move(r); });
  ASSERT_TRUE(checkout.success);
  EXPECT_GT(checkout.checkout_version, owner.ownership_version);
  db_.ReleaseCheckout(500, 1, owner, [](Result<void> r) { EXPECT_FALSE(r.HasValue()); });
}

TEST_F(MysqlDatabaseTest, CheckoutVersionExhaustionCannotWrapAndZeroCannotRelease) {
  PutResult put;
  db_.PutEntity(99, 1, WriteFlags::kCreateNew | WriteFlags::kExplicitDbid, make_blob("one"), "",
                [&](PutResult r) { put = std::move(r); });
  ASSERT_TRUE(put.success);
  CheckoutInfo owner{Address(0x7F000001, 7100), 2, 3};
  GetResult checkout;
  db_.CheckoutEntity(99, 1, owner, [&](GetResult r) { checkout = std::move(r); });
  ASSERT_TRUE(checkout.success);
  db_.ReleaseCheckout(99, 1, owner, [](Result<void> r) { EXPECT_FALSE(r.HasValue()); });
  owner.ownership_version = checkout.checkout_version;
  auto wrong_owner = owner;
  ++wrong_owner.app_id;
  db_.ReleaseCheckout(99, 1, wrong_owner, [](Result<void> r) { EXPECT_FALSE(r.HasValue()); });
  wrong_owner = owner;
  ++wrong_owner.entity_id;
  db_.ReleaseCheckout(99, 1, wrong_owner, [](Result<void> r) { EXPECT_FALSE(r.HasValue()); });
  wrong_owner = owner;
  wrong_owner.base_addr = Address(0x7F000001, 7101);
  db_.ReleaseCheckout(99, 1, wrong_owner, [](Result<void> r) { EXPECT_FALSE(r.HasValue()); });
  db_.ReleaseCheckout(99, 1, owner, [](Result<void> r) { EXPECT_TRUE(r.HasValue()); });
  auto admin = connect_admin(cfg_);
  ASSERT_NE(admin, nullptr);
  ASSERT_EQ(mysql_query(admin.get(),
                        "UPDATE atlas_checkout_versions "
                        "SET version = 18446744073709551615 WHERE dbid = 99"),
            0);
  db_.CheckoutEntity(99, 1, owner, [&](GetResult r) { checkout = std::move(r); });
  EXPECT_FALSE(checkout.success);
  EXPECT_EQ(checkout.checkout_version, 0u);
  GetResult loaded;
  db_.GetEntity(99, 1, [&](GetResult r) { loaded = std::move(r); });
  ASSERT_TRUE(loaded.success);
  EXPECT_FALSE(loaded.checked_out_by.has_value());
}

TEST_F(MysqlDatabaseTest, ConcurrentCheckoutsIssueOneOwnershipVersion) {
  PutResult put;
  db_.PutEntity(0, 1, WriteFlags::kCreateNew, make_blob("shared"), "concurrent",
                [&](PutResult r) { put = std::move(r); });
  ASSERT_TRUE(put.success);
  MysqlDatabase peer;
  ASSERT_TRUE(peer.Startup(cfg_, EntityDefRegistry::Instance()).HasValue());
  db_.SetDeferredMode(true);
  peer.SetDeferredMode(true);
  std::vector<GetResult> results;
  CheckoutInfo first{Address(0x7F000001, 7100), 1, 2};
  CheckoutInfo second{Address(0x7F000001, 7101), 2, 3};
  db_.CheckoutEntity(put.dbid, 1, first, [&](GetResult r) { results.push_back(std::move(r)); });
  peer.CheckoutEntityByName(1, "concurrent", second,
                            [&](GetResult r) { results.push_back(std::move(r)); });
  db_.Shutdown();
  peer.Shutdown();
  ASSERT_EQ(results.size(), 2u);
  int winners = 0;
  uint64_t version = 0;
  for (const auto& result : results) {
    ASSERT_TRUE(result.success) << result.error;
    if (!result.checked_out_by) {
      ++winners;
      version = result.checkout_version;
    }
  }
  EXPECT_EQ(winners, 1);
  EXPECT_GT(version, 0u);
  for (const auto& result : results) {
    if (result.checked_out_by) {
      EXPECT_EQ(result.checked_out_by->ownership_version, version);
    }
  }
}

TEST_F(MysqlDatabaseTest, CheckedOutEntityRejectsWriteWithoutOwnerVersion) {
  PutResult put;
  db_.PutEntity(0, 1, WriteFlags::kCreateNew, make_blob("saved"), "leased",
                [&](PutResult r) { put = std::move(r); });
  ASSERT_TRUE(put.success);
  db_.CheckoutEntity(put.dbid, 1, CheckoutInfo{Address(0x7F000001, 7100), 1, 2},
                     [](GetResult r) { ASSERT_TRUE(r.success); });
  DatabaseWriteOperation write;
  write.id[0] = std::byte{91};
  write.requested_dbid = put.dbid;
  db_.PutEntityWithOperation(write, put.dbid, 1, WriteFlags::kNone, make_blob("stale"), "",
                             [](PutResult r) {
                               EXPECT_FALSE(r.success);
                               EXPECT_EQ(r.error_kind, DatabaseErrorKind::kOwnershipConflict);
                             });
}

TEST_F(MysqlDatabaseTest, FirstSaveOwnsEntityBeforeAnotherBackendCanCheckout) {
  DatabaseWriteOperation create;
  create.id[0] = std::byte{101};
  create.requested_flags = WriteFlags::kCreateNew;
  create.entity_id = 1001;
  create.create_owner = CheckoutInfo{Address(0x7F000001, 7100), 7, 1001};
  PutResult saved;
  db_.PutEntityWithOperation(create, 0, 1, WriteFlags::kCreateNew, make_blob("first"), "owned_save",
                             [&](PutResult r) { saved = std::move(r); });
  ASSERT_TRUE(saved.success);
  ASSERT_TRUE(saved.checkout_owner.has_value());
  EXPECT_GT(saved.checkout_owner->ownership_version, 0u);
  MysqlDatabase peer;
  ASSERT_TRUE(peer.Startup(cfg_, EntityDefRegistry::Instance()).HasValue());
  GetResult competing;
  peer.CheckoutEntity(saved.dbid, 1, CheckoutInfo{Address(0x7F000001, 7101), 8, 2002},
                      [&](GetResult r) { competing = std::move(r); });
  ASSERT_TRUE(competing.success);
  ASSERT_TRUE(competing.checked_out_by.has_value());
  EXPECT_EQ(competing.checked_out_by->ownership_version, saved.checkout_owner->ownership_version);
  DatabaseWriteOperation update;
  update.id[0] = std::byte{102};
  update.requested_dbid = saved.dbid;
  update.expected_owner = saved.checkout_owner;
  PutResult updated;
  peer.PutEntityWithOperation(update, saved.dbid, 1, WriteFlags::kNone, make_blob("updated"), "",
                              [&](PutResult r) { updated = std::move(r); });
  EXPECT_TRUE(updated.success);
  peer.Shutdown();
  db_.Shutdown();
  ASSERT_TRUE(db_.Startup(cfg_, EntityDefRegistry::Instance()).HasValue());
  PutResult replay;
  db_.PutEntityWithOperation(create, 0, 1, WriteFlags::kCreateNew, make_blob("first"), "owned_save",
                             [&](PutResult r) { replay = std::move(r); });
  ASSERT_TRUE(replay.success);
  EXPECT_EQ(replay.dbid, saved.dbid);
  ASSERT_TRUE(replay.checkout_owner.has_value());
  EXPECT_EQ(replay.checkout_owner->ownership_version, saved.checkout_owner->ownership_version);
}

TEST_F(MysqlDatabaseTest, FailedFirstSaveRollsBackDataLeaseVersionAndReceipt) {
  auto admin = connect_admin(cfg_);
  ASSERT_NE(admin, nullptr);
  ASSERT_EQ(mysql_query(admin.get(),
                        "CREATE TRIGGER reject_first_receipt BEFORE UPDATE ON atlas_write_receipts "
                        "FOR EACH ROW SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'injected'"),
            0);
  DatabaseWriteOperation create;
  create.id[0] = std::byte{103};
  create.requested_flags = WriteFlags::kCreateNew;
  create.entity_id = 1001;
  create.create_owner = CheckoutInfo{Address(0x7F000001, 7100), 7, 1001};
  PutResult saved;
  db_.PutEntityWithOperation(create, 0, 1, WriteFlags::kCreateNew, make_blob("first"),
                             "atomic_first", [&](PutResult r) { saved = std::move(r); });
  EXPECT_FALSE(saved.success);
  EXPECT_FALSE(saved.checkout_owner.has_value());
  LookupResult lookup;
  db_.LookupByName(1, "atomic_first", [&](LookupResult r) { lookup = std::move(r); });
  EXPECT_FALSE(lookup.found);
  ASSERT_EQ(mysql_query(admin.get(), "SELECT COUNT(*) FROM atlas_checkout_versions"), 0);
  {
    std::unique_ptr<MYSQL_RES, decltype(&mysql_free_result)> rows(mysql_store_result(admin.get()),
                                                                  mysql_free_result);
    ASSERT_NE(rows, nullptr);
    auto row = mysql_fetch_row(rows.get());
    ASSERT_NE(row, nullptr);
    EXPECT_STREQ(row[0], "0");
  }
  ASSERT_EQ(mysql_query(admin.get(), "DROP TRIGGER reject_first_receipt"), 0);
  db_.PutEntityWithOperation(create, 0, 1, WriteFlags::kCreateNew, make_blob("first"),
                             "atomic_first", [&](PutResult r) { saved = std::move(r); });
  ASSERT_TRUE(saved.success) << saved.error;
  ASSERT_TRUE(saved.checkout_owner.has_value());
  EXPECT_EQ(saved.checkout_owner->ownership_version, 1u);
}

TEST_F(MysqlDatabaseTest, FirstSaveRejectsInvalidCreatorAndReusedOperationForDifferentCreator) {
  DatabaseWriteOperation create;
  create.id[0] = std::byte{104};
  create.requested_flags = WriteFlags::kCreateNew;
  create.entity_id = 1001;
  create.create_owner = CheckoutInfo{Address(0x7F000001, 7100), 7, 1002};
  PutResult result;
  db_.PutEntityWithOperation(create, 0, 1, WriteFlags::kCreateNew, {}, "",
                             [&](PutResult r) { result = std::move(r); });
  EXPECT_FALSE(result.success);
  create.create_owner->entity_id = 1001;
  create.create_owner->ownership_version = 9;
  db_.PutEntityWithOperation(create, 0, 1, WriteFlags::kCreateNew, {}, "",
                             [&](PutResult r) { result = std::move(r); });
  EXPECT_FALSE(result.success);
  create.create_owner->ownership_version = 0;
  db_.PutEntityWithOperation(create, 0, 1, WriteFlags::kCreateNew, {}, "",
                             [&](PutResult r) { result = std::move(r); });
  ASSERT_TRUE(result.success) << result.error;
  create.create_owner->app_id = 8;
  db_.PutEntityWithOperation(create, 0, 1, WriteFlags::kCreateNew, {}, "",
                             [&](PutResult r) { result = std::move(r); });
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.error_kind, DatabaseErrorKind::kOperationConflict);
}

TEST_F(MysqlDatabaseTest, RetiringBeforeFirstRequestBlocksNameLookupAndFirstSave) {
  const CheckoutInfo owner{Address("127.0.0.1", 24991), 7, 9, 0, 111};
  db_.RetireCheckoutProcess(owner.base_addr, owner.process_instance, [](Result<int> result) {
    ASSERT_TRUE(result);
    EXPECT_EQ(*result, 0);
  });
  PutResult saved;
  db_.PutEntity(1800, 1, WriteFlags::kCreateNew | WriteFlags::kExplicitDbid, make_blob("original"),
                "retired_name", [&](PutResult result) { saved = std::move(result); });
  ASSERT_TRUE(saved.success) << saved.error;
  GetResult checked_out;
  db_.CheckoutEntityByName(1, "retired_name", owner,
                           [&](GetResult result) { checked_out = std::move(result); });
  EXPECT_FALSE(checked_out.success);
  EXPECT_EQ(checked_out.error_kind, DatabaseErrorKind::kOwnershipConflict);
  DatabaseWriteOperation create;
  create.id[0] = std::byte{0x7A};
  create.requested_flags = WriteFlags::kCreateNew;
  create.entity_id = owner.entity_id;
  create.create_owner = owner;
  db_.PutEntityWithOperation(create, 0, 1, WriteFlags::kCreateNew, make_blob("rejected"),
                             "retired_create",
                             [&](PutResult result) { saved = std::move(result); });
  EXPECT_FALSE(saved.success);
  EXPECT_EQ(saved.error_kind, DatabaseErrorKind::kOwnershipConflict);
  db_.LookupByName(1, "retired_create", [](LookupResult result) {
    EXPECT_TRUE(result.error.empty());
    EXPECT_FALSE(result.found);
  });
}

TEST_F(MysqlDatabaseTest, InvalidAndUnavailableProcessRetirementFailExplicitly) {
  const Address address("127.0.0.1", 24991);
  for (uint64_t instance : {uint64_t{0}, uint64_t{1} << 63}) {
    db_.RetireCheckoutProcess(address, instance, [](Result<int> result) {
      ASSERT_FALSE(result);
      EXPECT_EQ(result.Error().Code(), ErrorCode::kInvalidArgument);
    });
  }
  db_.Shutdown();
  db_.RetireCheckoutProcess(address, 111, [](Result<int> result) { EXPECT_FALSE(result); });
}

TEST_F(MysqlDatabaseTest, MissingProcessIdentityColumnRejectsStartup) {
  db_.Shutdown();
  auto admin = connect_admin(cfg_);
  ASSERT_NE(admin, nullptr);
  ASSERT_EQ(mysql_query(admin.get(), "ALTER TABLE entities DROP COLUMN checkout_instance"), 0);
  const auto result = db_.Startup(cfg_, EntityDefRegistry::Instance());
  EXPECT_FALSE(result);
}

TEST_F(MysqlDatabaseTest, RetirementFencesConcurrentAcquisitionsAcrossDatabaseInstances) {
  MysqlDatabase peer;
  ASSERT_TRUE(peer.Startup(cfg_, EntityDefRegistry::Instance()));
  for (int i = 0; i < 8; ++i) {
    PutResult created;
    db_.PutEntity(0, 1, WriteFlags::kCreateNew, make_blob("lease"), "",
                  [&](PutResult result) { created = std::move(result); });
    ASSERT_TRUE(created.success) << created.error;
    const CheckoutInfo owner{Address("127.0.0.1", 24991), 7, 9, 0, uint64_t{111} + i};
    db_.SetDeferredMode(true);
    peer.SetDeferredMode(true);
    bool acquired = false;
    bool retired = false;
    db_.CheckoutEntity(created.dbid, 1, owner, [&](GetResult) { acquired = true; });
    peer.RetireCheckoutProcess(owner.base_addr, owner.process_instance, [&](Result<int> result) {
      ASSERT_TRUE(result) << result.Error().Message();
      retired = true;
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while ((!acquired || !retired) && std::chrono::steady_clock::now() < deadline) {
      db_.ProcessResults();
      peer.ProcessResults();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_TRUE(acquired);
    ASSERT_TRUE(retired);
    db_.SetDeferredMode(false);
    peer.SetDeferredMode(false);
    db_.GetEntity(created.dbid, 1, [](GetResult result) {
      ASSERT_TRUE(result.success);
      EXPECT_FALSE(result.checked_out_by);
    });
    db_.CheckoutEntity(created.dbid, 1, owner, [](GetResult result) {
      EXPECT_FALSE(result.success);
      EXPECT_EQ(result.error_kind, DatabaseErrorKind::kOwnershipConflict);
    });
  }
  peer.Shutdown();
}
