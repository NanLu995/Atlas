#include <chrono>
#include <filesystem>
#include <format>
#include <limits>

#include <gtest/gtest.h>

#include "atdf_account_fixture.h"
#include "db_sqlite/sqlite_database.h"
#include "entitydef/entity_def_registry.h"
#include "test_account_session_contract.h"
#include "test_database_admission.h"

using namespace atlas;

namespace {

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

}  // namespace

namespace atlas {

struct SqliteDatabaseTestAccess {
  static auto Execute(SqliteDatabase& db, std::string_view sql) -> Result<void> {
    return db.ExecSql(sql);
  }
  static void RejectCommits(SqliteDatabase& db, bool reject) {
    sqlite3_commit_hook(db.db_, reject ? Reject : nullptr, nullptr);
  }

 private:
  static int Reject(void*) { return 1; }
};

}  // namespace atlas

TEST(SqliteOwnership, RejectsSecondInstanceAndAllowsRestart) {
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto directory =
      std::filesystem::temp_directory_path() / std::format("atlas_sqlite_ownership_{}", stamp);
  ASSERT_TRUE(std::filesystem::create_directory(directory));
  DatabaseConfig config;
  config.type = "sqlite";
  config.sqlite_path = directory / "single.sqlite3";
  SqliteDatabase first;
  SqliteDatabase second;
  auto started = first.Startup(config, EntityDefRegistry::Instance());
  ASSERT_TRUE(started.HasValue()) << started.Error().Message();
  auto rejected = second.Startup(config, EntityDefRegistry::Instance());
  EXPECT_FALSE(rejected.HasValue());
  if (!rejected) {
    EXPECT_NE(rejected.Error().Message().find("single DBApp"), std::string_view::npos);
  }
  second.Shutdown();
  first.Shutdown();
  ASSERT_TRUE(second.Startup(config, EntityDefRegistry::Instance()));
  second.Shutdown();
}

class SqliteDatabaseTest : public ::testing::Test {
 protected:
  void SetUp() override {
    register_account_entity();

    test_dir_ = std::filesystem::temp_directory_path() / "atlas_sqlite_db_test";
    std::filesystem::remove_all(test_dir_);
    std::filesystem::create_directories(test_dir_);

    DatabaseConfig cfg;
    cfg.type = "sqlite";
    cfg.sqlite_path = test_dir_ / "atlas.sqlite3";
    cfg.sqlite_wal = true;
    cfg.sqlite_busy_timeout_ms = 1000;
    cfg.sqlite_foreign_keys = true;

    auto start = db_.Startup(cfg, EntityDefRegistry::Instance());
    ASSERT_TRUE(start.HasValue()) << "db startup failed: " << start.Error().Message();
  }

  void TearDown() override {
    db_.Shutdown();
    EntityDefRegistry::Instance().clear();
    std::filesystem::remove_all(test_dir_);
  }

  SqliteDatabase db_;
  std::filesystem::path test_dir_;
};

TEST_F(SqliteDatabaseTest, AdmissionIncludesUnconsumedResultsAndRejectsBeforeWriting) {
  atlas::test_support::CheckDatabaseAdmission(db_);
}

TEST_F(SqliteDatabaseTest, StartupRejectsSchemaVersionWithoutOverwritingIt) {
  ASSERT_TRUE(SqliteDatabaseTestAccess::Execute(
      db_, "UPDATE meta SET value = 'unsupported' WHERE key = 'schema_version'"));
  db_.Shutdown();

  DatabaseConfig config;
  config.type = "sqlite";
  config.sqlite_path = test_dir_ / "atlas.sqlite3";
  auto started = db_.Startup(config, EntityDefRegistry::Instance());
  EXPECT_FALSE(started.HasValue());
  if (!started) {
    EXPECT_NE(started.Error().Message().find("schema_version"), std::string_view::npos);
  }
  db_.Shutdown();

  sqlite3* raw = nullptr;
  ASSERT_EQ(sqlite3_open(config.sqlite_path.string().c_str(), &raw), SQLITE_OK);
  std::unique_ptr<sqlite3, decltype(&sqlite3_close)> connection(raw, sqlite3_close);
  std::string version;
  ASSERT_EQ(sqlite3_exec(
                raw, "SELECT value FROM meta WHERE key = 'schema_version'",
                [](void* output, int, char** values, char**) {
                  *static_cast<std::string*>(output) = values[0];
                  return 0;
                },
                &version, nullptr),
            SQLITE_OK);
  EXPECT_EQ(version, "unsupported");
}

TEST_F(SqliteDatabaseTest, StartupRejectsMissingSchemaVersion) {
  ASSERT_TRUE(
      SqliteDatabaseTestAccess::Execute(db_, "DELETE FROM meta WHERE key = 'schema_version'"));
  db_.Shutdown();
  DatabaseConfig config;
  config.type = "sqlite";
  config.sqlite_path = test_dir_ / "atlas.sqlite3";
  auto started = db_.Startup(config, EntityDefRegistry::Instance());
  ASSERT_FALSE(started.HasValue());
  EXPECT_NE(started.Error().Message().find("schema_version"), std::string_view::npos);
}

TEST_F(SqliteDatabaseTest, StartupRejectsUnsupportedBlobFormatWithoutUpdatingMetadata) {
  ASSERT_TRUE(SqliteDatabaseTestAccess::Execute(
      db_, "UPDATE meta SET value = 'unsupported' WHERE key = 'blob_format_version'"));
  db_.Shutdown();
  DatabaseConfig config;
  config.type = "sqlite";
  config.sqlite_path = test_dir_ / "atlas.sqlite3";
  auto started = db_.Startup(config, EntityDefRegistry::Instance());
  ASSERT_FALSE(started.HasValue());
  EXPECT_NE(started.Error().Message().find("blob_format_version"), std::string_view::npos);
}

TEST_F(SqliteDatabaseTest, StartupRejectsChangedSnapshotSchemaWithoutReplacingTheDigest) {
  db_.Shutdown();
  DatabaseConfig config;
  config.type = "sqlite";
  config.sqlite_path = test_dir_ / "atlas.sqlite3";
  auto& registry = EntityDefRegistry::Instance();
  const auto original = registry.types[0].properties[1].data_type;
  registry.types[0].properties[1].data_type = PropertyDataType::kUInt64;
  auto started = db_.Startup(config, registry);
  EXPECT_FALSE(started.HasValue());
  if (!started) {
    EXPECT_NE(started.Error().Message().find("storage_schema_digest"), std::string_view::npos);
  }
  db_.Shutdown();
  registry.types[0].properties[1].data_type = original;
  ASSERT_TRUE(db_.Startup(config, registry));
}

TEST_F(SqliteDatabaseTest, StartupRejectsMissingSnapshotSchemaDigest) {
  ASSERT_TRUE(SqliteDatabaseTestAccess::Execute(
      db_, "DELETE FROM meta WHERE key = 'storage_schema_digest'"));
  db_.Shutdown();
  DatabaseConfig config;
  config.type = "sqlite";
  config.sqlite_path = test_dir_ / "atlas.sqlite3";
  auto started = db_.Startup(config, EntityDefRegistry::Instance());
  ASSERT_FALSE(started.HasValue());
  EXPECT_NE(started.Error().Message().find("storage_schema_digest"), std::string_view::npos);
}

TEST_F(SqliteDatabaseTest, DrainRejectsNewWorkAndCompletesAcceptedCallbacks) {
  atlas::test_support::CheckDatabaseDrain(db_);
}

TEST_F(SqliteDatabaseTest, AccountSessionOwnershipContract) {
  atlas::test_support::CheckAccountSessionContract(db_);
}

TEST_F(SqliteDatabaseTest, SessionRecoveryClaimRequiresConfirmedRetirementAndCheckouts) {
  atlas::test_support::CheckSessionRecoveryClaim(db_);
}

TEST_F(SqliteDatabaseTest, SessionClaimAcceptsAProcessCheckoutBeforeEntityAllocation) {
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

TEST_F(SqliteDatabaseTest, AccountSessionMustBeReleasedBeforeDeletingAccount) {
  atlas::test_support::CheckSessionDeletionFence(db_);
}

TEST_F(SqliteDatabaseTest, AccountSessionAbortFencesLateCompletion) {
  atlas::test_support::CheckSessionAbortContract(db_);
}

TEST_F(SqliteDatabaseTest, AccountSessionRetirementRejectsReceiptReplay) {
  atlas::test_support::CheckRetiredSessionReplay(db_);
}

TEST_F(SqliteDatabaseTest, AccountSessionCommitFailureCannotGrantControl) {
  auto claim = atlas::test_support::CreateSessionAccount(db_);
  ASSERT_TRUE(claim.HasValue());
  atlas::SqliteDatabaseTestAccess::RejectCommits(db_, true);
  auto failed = atlas::test_support::UpdateSession(db_, *claim);
  atlas::SqliteDatabaseTestAccess::RejectCommits(db_, false);
  ASSERT_FALSE(failed.HasValue());
  EXPECT_EQ(failed.Error().Code(), ErrorCode::kCommitUnknown);
  db_.ReadAccountSession(claim->account_dbid, 1, [](Result<AccountSession> value) {
    ASSERT_TRUE(value.HasValue());
    EXPECT_EQ(value->version, 0u);
  });
  auto replay = atlas::test_support::UpdateSession(db_, *claim);
  ASSERT_TRUE(replay.HasValue());
  EXPECT_EQ(replay->version, 1u);
}

TEST_F(SqliteDatabaseTest, AccountSessionBatchCallbackWaitsForCommit) {
  auto claim = atlas::test_support::CreateSessionAccount(db_);
  ASSERT_TRUE(claim.HasValue());
  bool called = false;
  db_.BeginBatch();
  db_.UpdateAccountSession(*claim, [&](Result<AccountSession> value) {
    called = true;
    ASSERT_TRUE(value.HasValue());
    EXPECT_EQ(value->version, 1u);
  });
  db_.ProcessResults();
  EXPECT_FALSE(called);
  db_.EndBatch();
  db_.ProcessResults();
  EXPECT_TRUE(called);
}

TEST_F(SqliteDatabaseTest, AccountSessionBatchCommitFailureInvalidatesCallback) {
  auto claim = atlas::test_support::CreateSessionAccount(db_);
  ASSERT_TRUE(claim.HasValue());
  bool called = false;
  db_.BeginBatch();
  db_.UpdateAccountSession(*claim, [&](Result<AccountSession> value) {
    called = true;
    ASSERT_FALSE(value.HasValue());
    EXPECT_EQ(value.Error().Code(), ErrorCode::kCommitUnknown);
  });
  atlas::SqliteDatabaseTestAccess::RejectCommits(db_, true);
  db_.EndBatch();
  atlas::SqliteDatabaseTestAccess::RejectCommits(db_, false);
  db_.ProcessResults();
  EXPECT_TRUE(called);
  db_.ReadAccountSession(claim->account_dbid, 1, [](Result<AccountSession> value) {
    ASSERT_TRUE(value.HasValue());
    EXPECT_EQ(value->version, 0u);
  });
}

TEST_F(SqliteDatabaseTest, CorruptedAccountSessionIsNotAnOfflineGrant) {
  auto claim = atlas::test_support::CreateSessionAccount(db_);
  ASSERT_TRUE(claim.HasValue());
  ASSERT_TRUE(atlas::test_support::UpdateSession(db_, *claim).HasValue());
  ASSERT_TRUE(atlas::SqliteDatabaseTestAccess::Execute(
      db_, "UPDATE atlas_account_sessions SET payload=X'00'"));
  db_.ReadAccountSession(claim->account_dbid, 1, [](Result<AccountSession> value) {
    ASSERT_FALSE(value.HasValue());
    EXPECT_EQ(value.Error().Code(), ErrorCode::kIoError);
  });
  EXPECT_FALSE(atlas::test_support::UpdateSession(db_, *claim).HasValue());
}

TEST(AccountSessionFormat, TruncationInvalidGrantAndExhaustionAreRejected) {
  AccountSession session;
  session.version = 1;
  session.phase = AccountSessionPhase::kActive;
  session.owner = {Address(0x7F000001u, 31000), 100, 101};
  BinaryWriter writer;
  WriteAccountSession(writer, session);
  for (std::size_t size = 0; size < writer.Data().size(); ++size) {
    BinaryReader reader(writer.Data().first(size));
    EXPECT_FALSE(ReadAccountSession(reader).HasValue()) << size;
  }
  BinaryReader reader(writer.Data());
  auto decoded = ReadAccountSession(reader);
  ASSERT_TRUE(decoded.HasValue());
  EXPECT_EQ(*decoded, session);
  session.target = session.owner;
  EXPECT_FALSE(ValidateAccountSession(session).HasValue());
  session.target = {};
  session.version = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
  AccountSessionOperation release;
  release.id[0] = std::byte{1};
  release.account_dbid = 1;
  release.account_type_id = 1;
  release.actor = session.owner;
  release.expected_version = session.version;
  release.action = AccountSessionAction::kRelease;
  const auto exhausted = ApplySessionOperation(session, release);
  ASSERT_FALSE(exhausted.HasValue());
  EXPECT_EQ(exhausted.Error().Code(), ErrorCode::kOutOfRange);
}

TEST_F(SqliteDatabaseTest, PutNewEntityAndGet) {
  PutResult put;
  db_.PutEntity(kInvalidDBID, 1, WriteFlags::kCreateNew, make_blob("hello"), "alice",
                [&](PutResult r) { put = std::move(r); });

  ASSERT_TRUE(put.success);
  EXPECT_GT(put.dbid, 0);

  GetResult get;
  db_.GetEntity(put.dbid, 1, [&](GetResult r) { get = std::move(r); });
  ASSERT_TRUE(get.success);
  EXPECT_EQ(get.data.blob, make_blob("hello"));
  EXPECT_EQ(get.data.identifier, "alice");
}

TEST_F(SqliteDatabaseTest, OperationWriteAndDeleteFenceStaleAndMissingOwners) {
  db_.PutEntity(42, 1, WriteFlags::kCreateNew | WriteFlags::kExplicitDbid, make_blob("saved"),
                "owned", [](PutResult r) { ASSERT_TRUE(r.success) << r.error; });
  CheckoutInfo owner{Address("127.0.0.1", 10001), 3, 9};
  db_.CheckoutEntity(42, 1, owner, [&](GetResult r) {
    ASSERT_TRUE(r.success) << r.error;
    owner.ownership_version = r.checkout_version;
  });
  DatabaseWriteOperation operation;
  operation.id[0] = std::byte{1};
  operation.requested_dbid = 42;
  operation.entity_id = owner.entity_id;
  for (auto flags : {WriteFlags::kNone, WriteFlags::kLogOff, WriteFlags::kDelete}) {
    operation.requested_flags = flags;
    for (int field = 0; field < 6; ++field) {
      operation.expected_owner = owner;
      if (field == 0) operation.expected_owner.reset();
      if (field == 1) operation.expected_owner->ownership_version = 0;
      if (field == 2) ++operation.expected_owner->ownership_version;
      if (field == 3) ++operation.expected_owner->app_id;
      if (field == 4) ++operation.expected_owner->entity_id;
      if (field == 5) operation.expected_owner->base_addr = Address("127.0.0.1", 10002);
      db_.PutEntityWithOperation(operation, 42, 1, flags, make_blob("stale"), "", [](PutResult r) {
        EXPECT_FALSE(r.success);
        EXPECT_EQ(r.error_kind, DatabaseErrorKind::kOwnershipConflict);
      });
    }
  }
  db_.GetEntity(42, 1, [&](GetResult r) {
    ASSERT_TRUE(r.success) << r.error;
    ASSERT_TRUE(r.checked_out_by);
    EXPECT_EQ(r.checked_out_by->ownership_version, owner.ownership_version);
    EXPECT_EQ(r.data.blob, make_blob("saved"));
  });
  operation.expected_owner = owner;
  operation.requested_flags = WriteFlags::kNone;
  db_.PutEntityWithOperation(operation, 42, 1, WriteFlags::kNone, make_blob("updated"), "",
                             [](PutResult r) { EXPECT_TRUE(r.success) << r.error; });
  operation.id[0] = std::byte{2};
  operation.requested_flags = WriteFlags::kDelete;
  db_.PutEntityWithOperation(operation, 42, 1, WriteFlags::kDelete, {}, "", [](PutResult r) {
    EXPECT_TRUE(r.success) << r.error;
    EXPECT_EQ(r.dbid, 42);
  });
  db_.GetEntity(42, 1, [](GetResult r) {
    EXPECT_FALSE(r.success);
    EXPECT_EQ(r.error_kind, DatabaseErrorKind::kNotFound);
  });
}

TEST_F(SqliteDatabaseTest, FailedConditionalMutationCommitKeepsDataAndLease) {
  db_.PutEntity(42, 1, WriteFlags::kCreateNew | WriteFlags::kExplicitDbid, make_blob("saved"),
                "owned", [](PutResult r) { ASSERT_TRUE(r.success); });
  CheckoutInfo owner{Address("127.0.0.1", 10001), 3, 9};
  db_.CheckoutEntity(42, 1, owner, [&](GetResult r) {
    ASSERT_TRUE(r.success);
    owner.ownership_version = r.checkout_version;
  });
  DatabaseWriteOperation operation;
  operation.id[0] = std::byte{1};
  operation.requested_dbid = 42;
  operation.expected_owner = owner;
  SqliteDatabaseTestAccess::RejectCommits(db_, true);
  for (auto flags : {WriteFlags::kLogOff, WriteFlags::kDelete}) {
    operation.requested_flags = flags;
    db_.PutEntityWithOperation(operation, 42, 1, flags, make_blob("uncommitted"), "",
                               [](PutResult r) {
                                 EXPECT_FALSE(r.success);
                                 EXPECT_EQ(r.error_kind, DatabaseErrorKind::kCommitUnknown);
                               });
  }
  SqliteDatabaseTestAccess::RejectCommits(db_, false);
  db_.GetEntity(42, 1, [&](GetResult r) {
    ASSERT_TRUE(r.success);
    EXPECT_EQ(r.data.blob, make_blob("saved"));
    ASSERT_TRUE(r.checked_out_by);
    EXPECT_EQ(r.checked_out_by->ownership_version, owner.ownership_version);
  });
}

TEST_F(SqliteDatabaseTest, DefaultOperationImplementationRejectsUnsupportedWrites) {
  DatabaseWriteOperation operation;
  operation.id[0] = std::byte{1};
  operation.expected_owner = CheckoutInfo{Address("127.0.0.1", 10001), 3, 9, 1};
  const auto rejected = [](PutResult r) {
    EXPECT_FALSE(r.success);
    EXPECT_EQ(r.error, "write_operation_not_supported");
  };
  db_.IDatabase::PutEntityWithOperation(operation, 42, 1, WriteFlags::kNone, {}, "", rejected);
  operation.expected_owner.reset();
  db_.IDatabase::PutEntityWithOperation(operation, 42, 1, WriteFlags::kNone, {}, "", rejected);
}

TEST_F(SqliteDatabaseTest, FirstSavePersistsEntityAndOwnerBeforeReply) {
  DatabaseWriteOperation operation;
  operation.id[0] = std::byte{1};
  operation.requested_flags = WriteFlags::kCreateNew;
  operation.entity_id = 9;
  operation.create_owner = CheckoutInfo{Address("127.0.0.1", 10001), 3, 9};
  PutResult saved;
  db_.PutEntityWithOperation(operation, kInvalidDBID, 1, WriteFlags::kCreateNew, make_blob("first"),
                             "first", [&](PutResult r) { saved = std::move(r); });
  ASSERT_TRUE(saved.success) << saved.error;
  ASSERT_TRUE(saved.checkout_owner);
  EXPECT_GT(saved.checkout_owner->ownership_version, 0u);
  EXPECT_EQ(saved.checkout_owner->entity_id, operation.entity_id);
  db_.Shutdown();
  DatabaseConfig config;
  config.sqlite_path = test_dir_ / "atlas.sqlite3";
  ASSERT_TRUE(db_.Startup(config, EntityDefRegistry::Instance()).HasValue());
  db_.CheckoutEntity(saved.dbid, 1, CheckoutInfo{}, [&](GetResult r) {
    ASSERT_TRUE(r.success) << r.error;
    ASSERT_TRUE(r.checked_out_by);
    EXPECT_EQ(*r.checked_out_by, *saved.checkout_owner);
    EXPECT_EQ(r.data.blob, make_blob("first"));
  });
}

TEST_F(SqliteDatabaseTest, FirstSaveRetryAfterRestartUsesReceiptAndRejectsChangedRequest) {
  DatabaseWriteOperation operation;
  operation.id[0] = std::byte{1};
  operation.requested_flags = WriteFlags::kCreateNew;
  operation.entity_id = 9;
  operation.create_owner = CheckoutInfo{Address("127.0.0.1", 10001), 3, 9};
  PutResult first;
  db_.PutEntityWithOperation(operation, 0, 1, operation.requested_flags, make_blob("first"), "",
                             [&](PutResult r) { first = std::move(r); });
  ASSERT_TRUE(first.success) << first.error;
  db_.Shutdown();
  DatabaseConfig config;
  config.sqlite_path = test_dir_ / "atlas.sqlite3";
  ASSERT_TRUE(db_.Startup(config, EntityDefRegistry::Instance()).HasValue());
  db_.PutEntityWithOperation(operation, 0, 1, operation.requested_flags, make_blob("first"), "",
                             [&](PutResult r) {
                               ASSERT_TRUE(r.success) << r.error;
                               EXPECT_EQ(r.dbid, first.dbid);
                               EXPECT_EQ(r.checkout_owner, first.checkout_owner);
                             });
  db_.PutEntityWithOperation(operation, 0, 1, operation.requested_flags, make_blob("changed"), "",
                             [](PutResult r) {
                               EXPECT_FALSE(r.success);
                               EXPECT_EQ(r.error_kind, DatabaseErrorKind::kOperationConflict);
                             });
  db_.GetMaxDbidInRange(1, 100, [&](DbidRangeResult r) {
    ASSERT_TRUE(r.success);
    EXPECT_EQ(r.max_dbid, first.dbid);
  });
}

TEST_F(SqliteDatabaseTest, ReplayedLogoffReturnsOriginalResultAndKeepsNewLease) {
  db_.PutEntity(42, 1, WriteFlags::kCreateNew | WriteFlags::kExplicitDbid, make_blob("saved"), "",
                [](PutResult r) { ASSERT_TRUE(r.success); });
  CheckoutInfo owner{Address("127.0.0.1", 10001), 3, 9};
  db_.CheckoutEntity(42, 1, owner, [&](GetResult r) {
    ASSERT_TRUE(r.success);
    owner.ownership_version = r.checkout_version;
  });
  DatabaseWriteOperation operation;
  operation.id[0] = std::byte{2};
  operation.requested_dbid = 42;
  operation.requested_flags = WriteFlags::kLogOff;
  operation.expected_owner = owner;
  const auto save = [&] {
    db_.PutEntityWithOperation(operation, 42, 1, WriteFlags::kLogOff, make_blob("logoff"), "",
                               [](PutResult r) { ASSERT_TRUE(r.success) << r.error; });
  };
  save();
  ++owner.entity_id;
  db_.CheckoutEntity(42, 1, owner, [&](GetResult r) {
    ASSERT_TRUE(r.success);
    owner.ownership_version = r.checkout_version;
  });
  save();
  db_.GetEntity(42, 1, [&](GetResult r) {
    ASSERT_TRUE(r.success);
    EXPECT_EQ(r.checked_out_by, owner);
  });
}

TEST_F(SqliteDatabaseTest, ReceiptInsertFailureRollsBackEntityAndLease) {
  ASSERT_TRUE(SqliteDatabaseTestAccess::Execute(
                  db_,
                  "CREATE TRIGGER reject_receipt BEFORE INSERT ON atlas_write_receipts "
                  "BEGIN SELECT RAISE(ABORT, 'injected receipt failure'); END")
                  .HasValue());
  DatabaseWriteOperation operation;
  operation.id[0] = std::byte{1};
  operation.requested_dbid = 42;
  operation.requested_flags = WriteFlags::kCreateNew | WriteFlags::kExplicitDbid;
  operation.entity_id = 9;
  operation.create_owner = CheckoutInfo{Address("127.0.0.1", 10001), 3, 9};
  db_.PutEntityWithOperation(operation, 42, 1, operation.requested_flags, make_blob("saved"), "",
                             [](PutResult r) {
                               EXPECT_FALSE(r.success);
                               EXPECT_FALSE(r.checkout_owner);
                             });
  db_.GetEntity(42, 1, [](GetResult r) {
    EXPECT_FALSE(r.success);
    EXPECT_EQ(r.error_kind, DatabaseErrorKind::kNotFound);
  });
  ASSERT_TRUE(SqliteDatabaseTestAccess::Execute(db_, "DROP TRIGGER reject_receipt").HasValue());
  db_.PutEntityWithOperation(operation, 42, 1, operation.requested_flags, make_blob("saved"), "",
                             [](PutResult r) {
                               ASSERT_TRUE(r.success) << r.error;
                               ASSERT_TRUE(r.checkout_owner);
                               EXPECT_EQ(r.checkout_owner->ownership_version, 1u);
                             });
}

TEST_F(SqliteDatabaseTest, MalformedReceiptCannotBeTreatedAsANewOperation) {
  DatabaseWriteOperation operation;
  operation.id[0] = std::byte{1};
  operation.requested_flags = WriteFlags::kCreateNew;
  operation.entity_id = 9;
  operation.create_owner = CheckoutInfo{Address("127.0.0.1", 10001), 3, 9};
  db_.PutEntityWithOperation(operation, 0, 1, operation.requested_flags, make_blob("saved"), "",
                             [](PutResult r) { ASSERT_TRUE(r.success); });
  ASSERT_TRUE(SqliteDatabaseTestAccess::Execute(db_,
                                                "UPDATE atlas_write_receipts SET "
                                                "request_hash = '00000000000000000000000000000000'")
                  .HasValue());
  db_.PutEntityWithOperation(operation, 0, 1, operation.requested_flags, make_blob("saved"), "",
                             [](PutResult r) {
                               EXPECT_FALSE(r.success);
                               EXPECT_EQ(r.error_kind, DatabaseErrorKind::kCommitUnknown);
                             });
  db_.GetMaxDbidInRange(1, 100, [](DbidRangeResult r) { EXPECT_EQ(r.max_dbid, 1); });
}

TEST_F(SqliteDatabaseTest, PendingDuplicateReturnsSameIdentityAfterBatchCommit) {
  DatabaseWriteOperation operation;
  operation.id[0] = std::byte{1};
  operation.requested_flags = WriteFlags::kCreateNew;
  operation.entity_id = 9;
  operation.create_owner = CheckoutInfo{Address("127.0.0.1", 10001), 3, 9};
  db_.BeginBatch();
  int completed = 0;
  const auto complete = [&](PutResult r) {
    ++completed;
    ASSERT_TRUE(r.success) << r.error;
    EXPECT_EQ(r.dbid, 1);
    ASSERT_TRUE(r.checkout_owner);
    EXPECT_EQ(r.checkout_owner->ownership_version, 1u);
  };
  db_.PutEntityWithOperation(operation, 0, 1, operation.requested_flags, make_blob("saved"), "",
                             complete);
  db_.PutEntityWithOperation(operation, 0, 1, operation.requested_flags, make_blob("saved"), "",
                             complete);
  EXPECT_EQ(completed, 0);
  db_.EndBatch();
  EXPECT_EQ(completed, 2);
}

TEST_F(SqliteDatabaseTest, RetryAfterRangeChangeKeepsOriginalAssignedIdentity) {
  DatabaseWriteOperation operation;
  operation.id[0] = std::byte{1};
  operation.requested_flags = WriteFlags::kCreateNew;
  operation.entity_id = 9;
  operation.create_owner = CheckoutInfo{Address("127.0.0.1", 10001), 3, 9};
  for (DatabaseID candidate : {42, 99}) {
    db_.PutEntityWithOperation(operation, candidate, 1,
                               WriteFlags::kCreateNew | WriteFlags::kExplicitDbid,
                               make_blob("saved"), "", [](PutResult r) {
                                 ASSERT_TRUE(r.success) << r.error;
                                 EXPECT_EQ(r.dbid, 42);
                               });
  }
  db_.GetMaxDbidInRange(1, 100, [](DbidRangeResult r) { EXPECT_EQ(r.max_dbid, 42); });
}

TEST_F(SqliteDatabaseTest, FailedFirstSaveCommitDoesNotCreateOrIssueOwner) {
  DatabaseWriteOperation operation;
  operation.id[0] = std::byte{1};
  operation.requested_dbid = 42;
  operation.requested_flags = WriteFlags::kCreateNew | WriteFlags::kExplicitDbid;
  operation.entity_id = 9;
  operation.create_owner = CheckoutInfo{Address("127.0.0.1", 10001), 3, 9};
  SqliteDatabaseTestAccess::RejectCommits(db_, true);
  db_.PutEntityWithOperation(operation, 42, 1, operation.requested_flags, make_blob("first"), "",
                             [](PutResult r) {
                               EXPECT_FALSE(r.success);
                               EXPECT_FALSE(r.checkout_owner);
                               EXPECT_EQ(r.error_kind, DatabaseErrorKind::kCommitUnknown);
                             });
  SqliteDatabaseTestAccess::RejectCommits(db_, false);
  db_.GetEntity(42, 1, [](GetResult r) {
    EXPECT_FALSE(r.success);
    EXPECT_EQ(r.error_kind, DatabaseErrorKind::kNotFound);
  });
  db_.PutEntityWithOperation(operation, 42, 1, operation.requested_flags, make_blob("first"), "",
                             [](PutResult r) {
                               ASSERT_TRUE(r.success) << r.error;
                               ASSERT_TRUE(r.checkout_owner);
                               EXPECT_EQ(r.checkout_owner->ownership_version, 1u);
                             });
}

TEST_F(SqliteDatabaseTest, FirstSaveBatchWaitsForCommitAndDoesNotPublishFailedOwner) {
  DatabaseWriteOperation operation;
  operation.id[0] = std::byte{1};
  operation.requested_dbid = 42;
  operation.requested_flags = WriteFlags::kCreateNew | WriteFlags::kExplicitDbid;
  operation.entity_id = 9;
  operation.create_owner = CheckoutInfo{Address("127.0.0.1", 10001), 3, 9};
  db_.BeginBatch();
  int completed = 0;
  db_.PutEntityWithOperation(operation, 42, 1, operation.requested_flags, make_blob("first"), "",
                             [&](PutResult r) {
                               ++completed;
                               EXPECT_FALSE(r.success);
                               EXPECT_FALSE(r.checkout_owner);
                               EXPECT_EQ(r.error_kind, DatabaseErrorKind::kCommitUnknown);
                             });
  db_.GetEntity(42, 1, [&](GetResult r) {
    ++completed;
    EXPECT_FALSE(r.success);
    EXPECT_FALSE(r.checked_out_by);
  });
  EXPECT_EQ(completed, 0);
  SqliteDatabaseTestAccess::RejectCommits(db_, true);
  db_.EndBatch();
  SqliteDatabaseTestAccess::RejectCommits(db_, false);
  EXPECT_EQ(completed, 2);
  db_.GetEntity(42, 1, [](GetResult r) {
    EXPECT_FALSE(r.success);
    EXPECT_EQ(r.error_kind, DatabaseErrorKind::kNotFound);
  });
}

TEST_F(SqliteDatabaseTest, InvalidFirstSaveOwnerCannotCreateEntityOrConsumeVersion) {
  DatabaseWriteOperation valid;
  valid.id[0] = std::byte{1};
  valid.requested_dbid = 42;
  valid.requested_flags = WriteFlags::kCreateNew | WriteFlags::kExplicitDbid;
  valid.entity_id = 9;
  valid.create_owner = CheckoutInfo{Address("127.0.0.1", 10001), 3, 9};
  for (int field = 0; field < 7; ++field) {
    auto operation = valid;
    if (field == 0) operation.expected_owner = valid.create_owner;
    if (field == 1) operation.create_owner->base_addr = Address("127.0.0.1", 0);
    if (field == 2) operation.entity_id = 0;
    if (field == 3) ++operation.create_owner->entity_id;
    if (field == 4) operation.create_owner->ownership_version = 1;
    if (field == 5) operation.requested_flags = operation.requested_flags | WriteFlags::kLogOff;
    if (field == 6) operation.requested_flags = operation.requested_flags | WriteFlags::kDelete;
    db_.PutEntityWithOperation(operation, 42, 1, operation.requested_flags, {}, "",
                               [](PutResult r) {
                                 EXPECT_FALSE(r.success);
                                 EXPECT_FALSE(r.checkout_owner);
                                 EXPECT_EQ(r.error_kind, DatabaseErrorKind::kOperationConflict);
                               });
  }
  db_.PutEntityWithOperation(valid, 42, 1, valid.requested_flags, make_blob("first"), "",
                             [](PutResult r) {
                               ASSERT_TRUE(r.success) << r.error;
                               ASSERT_TRUE(r.checkout_owner);
                               EXPECT_EQ(r.checkout_owner->ownership_version, 1u);
                             });
}

TEST_F(SqliteDatabaseTest, FirstSaveVersionExhaustionRollsBackCreatedEntity) {
  ASSERT_TRUE(SqliteDatabaseTestAccess::Execute(
                  db_,
                  "INSERT INTO atlas_checkout_versions(dbid, type_id, version) "
                  "VALUES(42, 1, 9223372036854775807)")
                  .HasValue());
  DatabaseWriteOperation operation;
  operation.id[0] = std::byte{1};
  operation.requested_dbid = 42;
  operation.requested_flags = WriteFlags::kCreateNew | WriteFlags::kExplicitDbid;
  operation.entity_id = 9;
  operation.create_owner = CheckoutInfo{Address("127.0.0.1", 10001), 3, 9};
  db_.PutEntityWithOperation(operation, 42, 1, operation.requested_flags, make_blob("first"), "",
                             [](PutResult r) {
                               EXPECT_FALSE(r.success);
                               EXPECT_FALSE(r.checkout_owner);
                               EXPECT_EQ(r.error, "checkout version exhausted");
                             });
  db_.GetEntity(42, 1, [](GetResult r) {
    EXPECT_FALSE(r.success);
    EXPECT_EQ(r.error_kind, DatabaseErrorKind::kNotFound);
  });
}

TEST_F(SqliteDatabaseTest, FailedConditionalDeleteBatchCommitKeepsConfirmedLease) {
  db_.PutEntity(42, 1, WriteFlags::kCreateNew | WriteFlags::kExplicitDbid, make_blob("saved"),
                "owned", [](PutResult r) { ASSERT_TRUE(r.success); });
  CheckoutInfo owner{Address("127.0.0.1", 10001), 3, 9};
  db_.CheckoutEntity(42, 1, owner, [&](GetResult r) {
    ASSERT_TRUE(r.success);
    owner.ownership_version = r.checkout_version;
  });
  DatabaseWriteOperation operation;
  operation.id[0] = std::byte{1};
  operation.requested_dbid = 42;
  operation.requested_flags = WriteFlags::kDelete;
  operation.expected_owner = owner;
  db_.BeginBatch();
  bool called = false;
  db_.PutEntityWithOperation(operation, 42, 1, WriteFlags::kDelete, {}, "", [&](PutResult r) {
    called = true;
    EXPECT_FALSE(r.success);
    EXPECT_EQ(r.error_kind, DatabaseErrorKind::kCommitUnknown);
  });
  EXPECT_FALSE(called);
  SqliteDatabaseTestAccess::RejectCommits(db_, true);
  db_.EndBatch();
  SqliteDatabaseTestAccess::RejectCommits(db_, false);
  EXPECT_TRUE(called);
  db_.GetEntity(42, 1, [&](GetResult r) {
    ASSERT_TRUE(r.success);
    EXPECT_EQ(r.data.blob, make_blob("saved"));
    ASSERT_TRUE(r.checked_out_by);
    EXPECT_EQ(r.checked_out_by->ownership_version, owner.ownership_version);
  });
}

TEST_F(SqliteDatabaseTest, PutNewEntityWithExplicitDbidAndGet) {
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
  ASSERT_TRUE(auto_put.success);
  EXPECT_GT(auto_put.dbid, kDbid);
}

TEST_F(SqliteDatabaseTest, RetiredProcessCannotAcquireOrClearReusedAddressOwner) {
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
  DatabaseConfig config;
  config.type = "sqlite";
  config.sqlite_path = test_dir_ / "atlas.sqlite3";
  ASSERT_TRUE(db_.Startup(config, EntityDefRegistry::Instance()));
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

TEST_F(SqliteDatabaseTest, RangeRecoveryRetainsDeletedReceiptAndCheckoutIdsAfterRestart) {
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
  DatabaseConfig config;
  config.type = "sqlite";
  config.sqlite_path = test_dir_ / "atlas.sqlite3";
  ASSERT_TRUE(db_.Startup(config, EntityDefRegistry::Instance()));
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

TEST_F(SqliteDatabaseTest, GetMaxDbidInRangeReturnsExistingMaximum) {
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

TEST_F(SqliteDatabaseTest, CheckoutAndClear) {
  PutResult put;
  db_.PutEntity(kInvalidDBID, 1, WriteFlags::kCreateNew, make_blob("data"), "bob",
                [&](PutResult r) { put = std::move(r); });
  ASSERT_TRUE(put.success);

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

TEST_F(SqliteDatabaseTest, CheckoutVersionsSurviveRestartAndFenceOldRelease) {
  PutResult created;
  db_.PutEntity(0, 1, WriteFlags::kCreateNew, make_blob("saved"), "lease_owner",
                [&](PutResult result) { created = std::move(result); });
  ASSERT_TRUE(created.success);
  CheckoutInfo owner{Address("127.0.0.1", 10001), 3, 9};
  GetResult first;
  db_.CheckoutEntity(created.dbid, 1, owner, [&](GetResult result) { first = std::move(result); });
  ASSERT_TRUE(first.success);
  ASSERT_GT(first.checkout_version, 0u);
  owner.ownership_version = first.checkout_version;
  db_.ReleaseCheckout(created.dbid, 1, owner,
                      [](Result<void> result) { ASSERT_TRUE(result.HasValue()); });
  GetResult second;
  db_.CheckoutEntityByName(1, "lease_owner", owner,
                           [&](GetResult result) { second = std::move(result); });
  ASSERT_TRUE(second.success);
  ASSERT_EQ(second.checkout_version, first.checkout_version + 1);
  db_.ReleaseCheckout(created.dbid, 1, owner, [](Result<void> result) {
    ASSERT_FALSE(result.HasValue());
    EXPECT_EQ(result.Error().Code(), ErrorCode::kPermissionDenied);
  });
  db_.Shutdown();
  DatabaseConfig config;
  config.type = "sqlite";
  config.sqlite_path = test_dir_ / "atlas.sqlite3";
  ASSERT_TRUE(db_.Startup(config, EntityDefRegistry::Instance()).HasValue());
  GetResult loaded;
  db_.GetEntity(created.dbid, 1, [&](GetResult result) { loaded = std::move(result); });
  ASSERT_TRUE(loaded.success);
  ASSERT_TRUE(loaded.checked_out_by);
  EXPECT_EQ(loaded.checked_out_by->ownership_version, second.checkout_version);
  owner.ownership_version = second.checkout_version;
  db_.ReleaseCheckout(created.dbid, 1, owner,
                      [](Result<void> result) { ASSERT_TRUE(result.HasValue()); });
  db_.DelEntity(created.dbid, 1, [](DelResult result) { ASSERT_TRUE(result.success); });
  db_.PutEntity(created.dbid, 1, WriteFlags::kCreateNew | WriteFlags::kExplicitDbid,
                make_blob("recreated"), "lease_owner",
                [](PutResult result) { ASSERT_TRUE(result.success); });
  GetResult third;
  db_.CheckoutEntity(created.dbid, 1, owner, [&](GetResult result) { third = std::move(result); });
  ASSERT_TRUE(third.success);
  EXPECT_EQ(third.checkout_version, second.checkout_version + 1);
}

TEST_F(SqliteDatabaseTest, ConditionalReleaseChecksEveryOwnerField) {
  PutResult created;
  db_.PutEntity(0, 1, WriteFlags::kCreateNew, make_blob("saved"), "owner_fields",
                [&](PutResult result) { created = std::move(result); });
  ASSERT_TRUE(created.success);
  CheckoutInfo owner{Address("127.0.0.1", 10001), 3, 9};
  GetResult acquired;
  db_.CheckoutEntity(created.dbid, 1, owner,
                     [&](GetResult result) { acquired = std::move(result); });
  ASSERT_TRUE(acquired.success);
  owner.ownership_version = acquired.checkout_version;
  for (int field = 0; field < 6; ++field) {
    auto wrong = owner;
    if (field == 0) wrong.base_addr = Address("127.0.0.2", 10001);
    if (field == 1) wrong.base_addr = Address("127.0.0.1", 10002);
    if (field == 2) ++wrong.app_id;
    if (field == 3) ++wrong.entity_id;
    if (field == 4) wrong.ownership_version = 0;
    if (field == 5) wrong.ownership_version = UINT64_MAX;
    bool called = false;
    db_.ReleaseCheckout(created.dbid, 1, wrong, [&](Result<void> result) {
      called = true;
      EXPECT_FALSE(result.HasValue());
    });
    ASSERT_TRUE(called);
  }
  GetResult loaded;
  db_.GetEntity(created.dbid, 1, [&](GetResult result) { loaded = std::move(result); });
  ASSERT_TRUE(loaded.success);
  ASSERT_TRUE(loaded.checked_out_by);
  EXPECT_EQ(loaded.checked_out_by->ownership_version, owner.ownership_version);
  db_.ReleaseCheckout(created.dbid, 1, owner,
                      [](Result<void> result) { ASSERT_TRUE(result.HasValue()); });
  db_.ReleaseCheckout(created.dbid, 1, owner,
                      [](Result<void> result) { EXPECT_FALSE(result.HasValue()); });
}

TEST_F(SqliteDatabaseTest, CheckoutVersionExhaustionCannotWrap) {
  db_.PutEntity(42, 1, WriteFlags::kCreateNew | WriteFlags::kExplicitDbid, make_blob("saved"),
                "exhausted", [](PutResult result) { ASSERT_TRUE(result.success); });
  ASSERT_TRUE(SqliteDatabaseTestAccess::Execute(
                  db_,
                  "INSERT INTO atlas_checkout_versions(dbid, type_id, version) "
                  "VALUES(42, 1, 9223372036854775807)")
                  .HasValue());
  GetResult result;
  db_.CheckoutEntity(42, 1, CheckoutInfo{}, [&](GetResult value) { result = std::move(value); });
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.checkout_version, 0u);
  EXPECT_EQ(result.error, "checkout version exhausted");
  db_.GetEntity(42, 1, [&](GetResult value) { result = std::move(value); });
  ASSERT_TRUE(result.success);
  EXPECT_FALSE(result.checked_out_by);
  EXPECT_EQ(result.data.blob, make_blob("saved"));
}

TEST_F(SqliteDatabaseTest, FailedCheckoutAndReleaseCommitsPreserveConfirmedOwnership) {
  db_.PutEntity(42, 1, WriteFlags::kCreateNew | WriteFlags::kExplicitDbid, make_blob("saved"),
                "failed_commit", [](PutResult result) { ASSERT_TRUE(result.success); });
  CheckoutInfo owner{Address("127.0.0.1", 10001), 3, 9};
  SqliteDatabaseTestAccess::RejectCommits(db_, true);
  GetResult acquired;
  db_.CheckoutEntity(42, 1, owner, [&](GetResult value) { acquired = std::move(value); });
  EXPECT_FALSE(acquired.success);
  EXPECT_EQ(acquired.checkout_version, 0u);
  SqliteDatabaseTestAccess::RejectCommits(db_, false);
  db_.CheckoutEntity(42, 1, owner, [&](GetResult value) { acquired = std::move(value); });
  ASSERT_TRUE(acquired.success);
  ASSERT_EQ(acquired.checkout_version, 1u);
  owner.ownership_version = acquired.checkout_version;
  SqliteDatabaseTestAccess::RejectCommits(db_, true);
  db_.ReleaseCheckout(42, 1, owner, [](Result<void> value) {
    ASSERT_FALSE(value.HasValue());
    EXPECT_EQ(value.Error().Code(), ErrorCode::kCommitUnknown);
  });
  SqliteDatabaseTestAccess::RejectCommits(db_, false);
  GetResult loaded;
  db_.GetEntity(42, 1, [&](GetResult value) { loaded = std::move(value); });
  ASSERT_TRUE(loaded.success);
  ASSERT_TRUE(loaded.checked_out_by);
  EXPECT_EQ(loaded.checked_out_by->ownership_version, 1u);
  db_.ReleaseCheckout(42, 1, owner, [](Result<void> value) { ASSERT_TRUE(value.HasValue()); });
  db_.BeginBatch();
  bool called = false;
  db_.CheckoutEntity(42, 1, owner, [&](GetResult value) {
    called = true;
    EXPECT_FALSE(value.success);
    EXPECT_EQ(value.checkout_version, 0u);
  });
  EXPECT_FALSE(called);
  SqliteDatabaseTestAccess::RejectCommits(db_, true);
  db_.EndBatch();
  ASSERT_TRUE(called);
  SqliteDatabaseTestAccess::RejectCommits(db_, false);
  db_.CheckoutEntity(42, 1, owner, [&](GetResult value) { acquired = std::move(value); });
  ASSERT_TRUE(acquired.success);
  EXPECT_EQ(acquired.checkout_version, 2u);
}

TEST_F(SqliteDatabaseTest, CorruptStoredCheckoutVersionIsNotAnAbsentEntity) {
  db_.PutEntity(42, 1, WriteFlags::kCreateNew | WriteFlags::kExplicitDbid, make_blob("saved"),
                "corrupt_version", [](PutResult result) { ASSERT_TRUE(result.success); });
  ASSERT_TRUE(SqliteDatabaseTestAccess::Execute(
                  db_, "UPDATE entities SET checkout_version = -1 WHERE dbid = 42")
                  .HasValue());
  db_.GetEntity(42, 1, [](GetResult value) {
    EXPECT_FALSE(value.success);
    EXPECT_NE(value.error_kind, DatabaseErrorKind::kNotFound);
    EXPECT_EQ(value.error, "invalid stored checkout version");
  });
}

TEST_F(SqliteDatabaseTest, AutoLoadAndDeferredCallbacks) {
  db_.SetDeferredMode(true);

  PutResult put;
  db_.PutEntity(kInvalidDBID, 1, WriteFlags::kCreateNew | WriteFlags::kAutoLoadOn,
                make_blob("auto"), "carol", [&](PutResult r) { put = std::move(r); });

  EXPECT_FALSE(put.success);
  db_.ProcessResults();
  ASSERT_TRUE(put.success);

  std::vector<EntityData> entities;
  db_.GetAutoLoadEntities([&](Result<std::vector<EntityData>> rows) {
    ASSERT_TRUE(rows);
    entities = std::move(*rows);
  });
  EXPECT_TRUE(entities.empty());
  db_.ProcessResults();
  ASSERT_EQ(entities.size(), 1u);
  EXPECT_EQ(entities[0].identifier, "carol");
}

TEST_F(SqliteDatabaseTest, LookupByNameReturnsStoredPasswordHash) {
  PutResult put;
  db_.PutEntityWithPassword(kInvalidDBID, 1, WriteFlags::kCreateNew, make_blob("acct"), "diana",
                            "pw_hash_123", [&](PutResult r) { put = std::move(r); });
  ASSERT_TRUE(put.success);

  LookupResult lookup;
  db_.LookupByName(1, "diana", [&](LookupResult r) { lookup = std::move(r); });
  ASSERT_TRUE(lookup.found);
  EXPECT_EQ(lookup.dbid, put.dbid);
  EXPECT_EQ(lookup.password_hash, "pw_hash_123");
}

TEST_F(SqliteDatabaseTest, DuplicateAccountCreateKeepsOriginalIdentityAndCredentials) {
  PutResult created;
  db_.PutEntityWithPassword(0, 1, WriteFlags::kCreateNew, make_blob("original"), "taken",
                            "original_hash", [&](PutResult r) { created = std::move(r); });
  ASSERT_TRUE(created.success);
  PutResult duplicate;
  db_.PutEntityWithPassword(0, 1, WriteFlags::kCreateNew, make_blob("replacement"), "taken",
                            "replacement_hash", [&](PutResult r) { duplicate = std::move(r); });
  EXPECT_FALSE(duplicate.success);
  EXPECT_EQ(duplicate.error_kind, DatabaseErrorKind::kDuplicateIdentifier);
  LookupResult lookup;
  db_.LookupByName(1, "taken", [&](LookupResult r) { lookup = std::move(r); });
  ASSERT_TRUE(lookup.found);
  EXPECT_EQ(lookup.dbid, created.dbid);
  EXPECT_EQ(lookup.password_hash, "original_hash");
  GetResult entity;
  db_.GetEntity(created.dbid, 1, [&](GetResult r) { entity = std::move(r); });
  ASSERT_TRUE(entity.success);
  EXPECT_EQ(entity.data.blob, make_blob("original"));
}

TEST_F(SqliteDatabaseTest, ResolvingUnknownCheckoutNeverAcquiresAnEntity) {
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

TEST_F(SqliteDatabaseTest, NamedCheckoutRetryBindsIdentityAndOperationKind) {
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

TEST_F(SqliteDatabaseTest, CheckoutRetryKeepsOriginalLeaseAfterRestart) {
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
  DatabaseConfig config;
  config.sqlite_path = test_dir_ / "atlas.sqlite3";
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

TEST_F(SqliteDatabaseTest, CheckoutReceiptFailureRollsBackLeaseAndVersion) {
  db_.PutEntity(42, 1, WriteFlags::kCreateNew | WriteFlags::kExplicitDbid, {}, "owned",
                [](PutResult r) { ASSERT_TRUE(r.success); });
  ASSERT_TRUE(SqliteDatabaseTestAccess::Execute(
      db_,
      "CREATE TRIGGER reject_checkout_receipt BEFORE INSERT ON atlas_write_receipts "
      "BEGIN SELECT RAISE(ABORT, 'injected receipt failure'); END"));
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
  ASSERT_TRUE(SqliteDatabaseTestAccess::Execute(db_, "DROP TRIGGER reject_checkout_receipt"));
  db_.CheckoutWithOperation(operation, [&](GetResult r) { result = std::move(r); });
  ASSERT_TRUE(result.success) << result.error;
  EXPECT_EQ(result.checkout_version, 1u);
}

TEST_F(SqliteDatabaseTest, CheckoutDuplicateWaitsForBatchAndRejectsFailedCommit) {
  db_.PutEntity(42, 1, WriteFlags::kCreateNew | WriteFlags::kExplicitDbid, {}, "owned",
                [](PutResult r) { ASSERT_TRUE(r.success); });
  DatabaseCheckoutOperation operation;
  operation.id[0] = std::byte{91};
  operation.dbid = 42;
  operation.type_id = 1;
  operation.owner = CheckoutInfo{Address(0x7F000001, 7100), 3, 77};
  db_.SetDeferredMode(true);
  db_.BeginBatch();
  int completed = 0;
  for (int i = 0; i < 2; ++i) {
    db_.CheckoutWithOperation(operation, [&](GetResult r) {
      ++completed;
      EXPECT_FALSE(r.success);
      EXPECT_EQ(r.checkout_version, 0u);
      EXPECT_EQ(r.error_kind, DatabaseErrorKind::kCommitUnknown);
    });
  }
  db_.ProcessResults();
  EXPECT_EQ(completed, 0);
  SqliteDatabaseTestAccess::RejectCommits(db_, true);
  db_.EndBatch();
  SqliteDatabaseTestAccess::RejectCommits(db_, false);
  db_.ProcessResults();
  EXPECT_EQ(completed, 2);
  db_.SetDeferredMode(false);
  db_.CheckoutWithOperation(operation, [](GetResult r) {
    ASSERT_TRUE(r.success) << r.error;
    EXPECT_EQ(r.checkout_version, 1u);
  });
}

TEST_F(SqliteDatabaseTest, AccountLookupCannotConfirmUncommittedCredentials) {
  db_.SetDeferredMode(true);
  db_.BeginBatch();
  db_.PutEntity(0, 1, WriteFlags::kCreateNew, {}, "preceding_write",
                [](PutResult r) { EXPECT_FALSE(r.success); });
  bool put_called = false;
  db_.PutEntityWithPassword(0, 1, WriteFlags::kCreateNew, {}, "pending_account", "pending_hash",
                            [&](PutResult r) {
                              put_called = true;
                              EXPECT_FALSE(r.success);
                            });
  bool lookup_called = false;
  LookupResult lookup;
  db_.LookupByName(1, "pending_account", [&](LookupResult r) {
    lookup_called = true;
    lookup = std::move(r);
  });
  db_.ProcessResults();
  EXPECT_FALSE(put_called);
  EXPECT_FALSE(lookup_called);
  SqliteDatabaseTestAccess::RejectCommits(db_, true);
  db_.EndBatch();
  SqliteDatabaseTestAccess::RejectCommits(db_, false);
  db_.ProcessResults();
  EXPECT_TRUE(put_called);
  EXPECT_TRUE(lookup_called);
  EXPECT_FALSE(lookup.found);
  EXPECT_FALSE(lookup.error.empty());
  EXPECT_EQ(lookup.dbid, kInvalidDBID);
  EXPECT_TRUE(lookup.password_hash.empty());
}

TEST_F(SqliteDatabaseTest, PutEntityWithPasswordUsesExplicitDbid) {
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

TEST_F(SqliteDatabaseTest, CheckoutByNameAndClearCheckoutsForAddress) {
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

TEST_F(SqliteDatabaseTest, PutEntityWithPasswordPreservesAutoLoadAndClearsCheckoutOnLogoff) {
  PutResult put;
  db_.PutEntityWithPassword(kInvalidDBID, 1, WriteFlags::kCreateNew | WriteFlags::kAutoLoadOn,
                            make_blob("acct"), "eve", "pw_hash_1",
                            [&](PutResult r) { put = std::move(r); });
  ASSERT_TRUE(put.success);

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
  ASSERT_TRUE(update.success);
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

TEST_F(SqliteDatabaseTest, EntityIdsAreReservedBeforeRestart) {
  db_.ReserveEntityIds(100, [](Result<EntityIdRange> range) {
    ASSERT_TRUE(range.HasValue());
    EXPECT_EQ(range->first, 1u);
    EXPECT_EQ(range->last, 100u);
  });
  db_.Shutdown();
  DatabaseConfig cfg;
  cfg.sqlite_path = test_dir_ / "atlas.sqlite3";
  ASSERT_TRUE(db_.Startup(cfg, EntityDefRegistry::Instance()).HasValue());
  db_.ReserveEntityIds(1, [](Result<EntityIdRange> range) {
    ASSERT_TRUE(range.HasValue());
    EXPECT_EQ(range->first, 101u);
  });
}

TEST_F(SqliteDatabaseTest, EntityIdReservationWaitsForOpenBatch) {
  PutResult created;
  db_.PutEntity(0, 1, WriteFlags::kCreateNew, make_blob("initial"), "batch",
                [&](PutResult result) { created = std::move(result); });
  ASSERT_TRUE(created.success);
  db_.SetDeferredMode(true);
  db_.BeginBatch();
  bool written = false;
  db_.PutEntity(created.dbid, 1, WriteFlags::kNone, make_blob("data"), "batch",
                [&](PutResult result) { written = result.success; });
  bool reserved = false;
  db_.ReserveEntityIds(10, [&](Result<EntityIdRange> range) {
    ASSERT_TRUE(range.HasValue());
    reserved = true;
    EXPECT_EQ(range->last, 10u);
  });
  db_.ProcessResults();
  EXPECT_FALSE(written);
  EXPECT_FALSE(reserved);
  db_.EndBatch();
  db_.ProcessResults();
  EXPECT_TRUE(written);
  EXPECT_TRUE(reserved);
}

TEST_F(SqliteDatabaseTest, InvalidEntityIdReservationDoesNotAdvanceCounter) {
  db_.ReserveEntityIds(0, [](Result<EntityIdRange> range) { EXPECT_FALSE(range.HasValue()); });
  db_.ReserveEntityIds(kFirstLocalEntityID - 1, [](Result<EntityIdRange> range) {
    ASSERT_TRUE(range.HasValue());
    EXPECT_EQ(range->last, kFirstLocalEntityID - 1);
  });
  db_.ReserveEntityIds(1, [](Result<EntityIdRange> range) { EXPECT_FALSE(range.HasValue()); });
}

TEST_F(SqliteDatabaseTest, FailedBatchCommitCannotReportSuccessfulWrite) {
  PutResult created;
  db_.PutEntity(0, 1, WriteFlags::kCreateNew, make_blob("original"), "commit",
                [&](PutResult result) { created = std::move(result); });
  ASSERT_TRUE(created.success);
  db_.SetDeferredMode(true);
  db_.BeginBatch();
  bool completed = false;
  db_.PutEntity(created.dbid, 1, WriteFlags::kNone, make_blob("lost"), "commit",
                [&](PutResult result) {
                  completed = true;
                  EXPECT_FALSE(result.success);
                  EXPECT_EQ(result.error_kind, DatabaseErrorKind::kCommitUnknown);
                  EXPECT_FALSE(result.error.empty());
                });
  SqliteDatabaseTestAccess::RejectCommits(db_, true);
  db_.EndBatch();
  SqliteDatabaseTestAccess::RejectCommits(db_, false);
  db_.BeginBatch();
  db_.EndBatch();
  EXPECT_FALSE(completed);
  db_.ProcessResults();
  EXPECT_TRUE(completed);
  db_.SetDeferredMode(false);
  GetResult loaded;
  db_.GetEntity(created.dbid, 1, [&](GetResult result) { loaded = std::move(result); });
  ASSERT_TRUE(loaded.success);
  EXPECT_EQ(loaded.data.blob, make_blob("original"));
}

TEST_F(SqliteDatabaseTest, SynchronousBatchCallbackWaitsForCommit) {
  PutResult created;
  db_.PutEntity(0, 1, WriteFlags::kCreateNew, make_blob("original"), "sync",
                [&](PutResult result) { created = std::move(result); });
  ASSERT_TRUE(created.success);
  db_.BeginBatch();
  bool completed = false;
  db_.PutEntity(created.dbid, 1, WriteFlags::kNone, make_blob("updated"), "sync",
                [&](PutResult result) {
                  completed = true;
                  EXPECT_TRUE(result.success);
                });
  EXPECT_FALSE(completed);
  db_.EndBatch();
  EXPECT_TRUE(completed);
}

TEST_F(SqliteDatabaseTest, StandaloneCommitFailureCannotReportSuccessfulCheckout) {
  PutResult created;
  db_.PutEntity(0, 1, WriteFlags::kCreateNew, make_blob("original"), "checkout",
                [&](PutResult result) { created = std::move(result); });
  ASSERT_TRUE(created.success);
  SqliteDatabaseTestAccess::RejectCommits(db_, true);
  CheckoutInfo owner{Address("127.0.0.1", 10001), 1, 2};
  GetResult checked_out;
  db_.CheckoutEntity(created.dbid, 1, owner,
                     [&](GetResult result) { checked_out = std::move(result); });
  EXPECT_FALSE(checked_out.success);
  EXPECT_EQ(checked_out.error_kind, DatabaseErrorKind::kCommitUnknown);
  SqliteDatabaseTestAccess::RejectCommits(db_, false);
  GetResult loaded;
  db_.GetEntity(created.dbid, 1, [&](GetResult result) { loaded = std::move(result); });
  ASSERT_TRUE(loaded.success);
  EXPECT_FALSE(loaded.checked_out_by.has_value());
}

TEST_F(SqliteDatabaseTest, EntityIdReservationCommitFailureDoesNotIssueIds) {
  SqliteDatabaseTestAccess::RejectCommits(db_, true);
  db_.ReserveEntityIds(100, [](Result<EntityIdRange> range) {
    ASSERT_FALSE(range.HasValue());
    EXPECT_EQ(range.Error().Code(), ErrorCode::kCommitUnknown);
  });
  SqliteDatabaseTestAccess::RejectCommits(db_, false);
  db_.ReserveEntityIds(1, [](Result<EntityIdRange> range) {
    ASSERT_TRUE(range.HasValue());
    EXPECT_EQ(range->first, 1u);
  });
}

TEST_F(SqliteDatabaseTest, MissingEntityHasExplicitErrorKind) {
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

TEST_F(SqliteDatabaseTest, ClosedDatabaseIsNotReportedAsMissingEntity) {
  db_.Shutdown();
  GetResult result;
  db_.GetEntity(1, 1, [&](GetResult r) { result = std::move(r); });
  EXPECT_FALSE(result.success);
  EXPECT_NE(result.error_kind, DatabaseErrorKind::kNotFound);
  EXPECT_FALSE(result.error.empty());
  db_.CheckoutEntity(1, 1, CheckoutInfo{}, [&](GetResult r) { result = std::move(r); });
  EXPECT_FALSE(result.success);
  EXPECT_NE(result.error_kind, DatabaseErrorKind::kNotFound);
  EXPECT_FALSE(result.error.empty());
}

TEST_F(SqliteDatabaseTest, RestartDoesNotRecreateMissingEntityIdCounter) {
  db_.ReserveEntityIds(10, [](Result<EntityIdRange> range) { ASSERT_TRUE(range.HasValue()); });
  db_.Shutdown();
  sqlite3* connection = nullptr;
  ASSERT_EQ(sqlite3_open((test_dir_ / "atlas.sqlite3").string().c_str(), &connection), SQLITE_OK);
  ASSERT_EQ(
      sqlite3_exec(connection, "DELETE FROM atlas_entity_id_counter", nullptr, nullptr, nullptr),
      SQLITE_OK);
  ASSERT_EQ(sqlite3_close(connection), SQLITE_OK);
  DatabaseConfig config;
  config.type = "sqlite";
  config.sqlite_path = test_dir_ / "atlas.sqlite3";
  ASSERT_TRUE(db_.Startup(config, EntityDefRegistry::Instance()).HasValue());
  db_.ReserveEntityIds(1, [](Result<EntityIdRange> range) { EXPECT_FALSE(range.HasValue()); });
}

TEST_F(SqliteDatabaseTest, RetiringBeforeFirstRequestBlocksNameLookupAndFirstSave) {
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

TEST_F(SqliteDatabaseTest, InvalidAndUnavailableProcessRetirementFailExplicitly) {
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

TEST_F(SqliteDatabaseTest, MissingProcessIdentityColumnRejectsStartup) {
  db_.Shutdown();
  const auto path = test_dir_ / "atlas.sqlite3";
  sqlite3* connection = nullptr;
  ASSERT_EQ(sqlite3_open(path.string().c_str(), &connection), SQLITE_OK);
  ASSERT_EQ(sqlite3_exec(connection, "ALTER TABLE entities DROP COLUMN checkout_instance", nullptr,
                         nullptr, nullptr),
            SQLITE_OK);
  ASSERT_EQ(sqlite3_close(connection), SQLITE_OK);
  DatabaseConfig config;
  config.type = "sqlite";
  config.sqlite_path = path;
  EXPECT_FALSE(db_.Startup(config, EntityDefRegistry::Instance()));
}

TEST_F(SqliteDatabaseTest, ProcessRetirementWaitsForCommitAndRollsBackWithBatch) {
  PutResult created;
  db_.PutEntity(0, 1, WriteFlags::kCreateNew, make_blob("lease"), "lease",
                [&](PutResult result) { created = std::move(result); });
  ASSERT_TRUE(created.success);
  const CheckoutInfo owner{Address("127.0.0.1", 24991), 7, 9, 0, 111};
  db_.CheckoutEntity(created.dbid, 1, owner, [](GetResult result) { ASSERT_TRUE(result.success); });
  db_.BeginBatch();
  SqliteDatabaseTestAccess::RejectCommits(db_, true);
  bool completed = false;
  db_.RetireCheckoutProcess(owner.base_addr, owner.process_instance, [&](Result<int> result) {
    completed = true;
    ASSERT_FALSE(result);
    EXPECT_EQ(result.Error().Code(), ErrorCode::kCommitUnknown);
  });
  EXPECT_FALSE(completed);
  db_.EndBatch();
  EXPECT_TRUE(completed);
  SqliteDatabaseTestAccess::RejectCommits(db_, false);
  GetResult loaded;
  db_.GetEntity(created.dbid, 1, [&](GetResult result) { loaded = std::move(result); });
  ASSERT_TRUE(loaded.success);
  ASSERT_TRUE(loaded.checked_out_by);
  EXPECT_EQ(loaded.checked_out_by->process_instance, owner.process_instance);
  db_.RetireCheckoutProcess(owner.base_addr, owner.process_instance, [](Result<int> result) {
    ASSERT_TRUE(result);
    EXPECT_EQ(*result, 1);
  });
}
