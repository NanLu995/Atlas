#include <limits>

#include <gtest/gtest.h>

#include "db/account_session.h"
#include "db/database_factory.h"
#include "db/idatabase.h"
#include "platform/dynamic_library.h"
#include "platform/filesystem.h"
#include "platform/platform_config.h"

using namespace atlas;

TEST(DatabaseTypes, WriteFlagsHasFlag) {
  auto flags = static_cast<WriteFlags>(static_cast<uint8_t>(WriteFlags::kCreateNew) |
                                       static_cast<uint8_t>(WriteFlags::kAutoLoadOn));

  EXPECT_TRUE(HasFlag(flags, WriteFlags::kCreateNew));
  EXPECT_TRUE(HasFlag(flags, WriteFlags::kAutoLoadOn));
  EXPECT_FALSE(HasFlag(flags, WriteFlags::kLogOff));
  EXPECT_FALSE(HasFlag(flags, WriteFlags::kDelete));
}

TEST(DatabaseTypes, WriteFlagsAndOp) {
  auto a = WriteFlags::kCreateNew;
  auto b = WriteFlags::kLogOff;
  auto combined = static_cast<WriteFlags>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));

  EXPECT_TRUE(HasFlag(combined & WriteFlags::kCreateNew, WriteFlags::kCreateNew));
  EXPECT_FALSE(HasFlag(combined & WriteFlags::kDelete, WriteFlags::kDelete));
}

TEST(DatabaseTypes, EntityDataDefaults) {
  EntityData d;
  EXPECT_EQ(d.dbid, kInvalidDBID);
  EXPECT_EQ(d.type_id, 0u);
  EXPECT_TRUE(d.blob.empty());
  EXPECT_TRUE(d.identifier.empty());
}

TEST(DatabaseTypes, ResultDefaults) {
  PutResult put;
  EXPECT_FALSE(put.success);
  EXPECT_EQ(put.dbid, kInvalidDBID);

  GetResult get;
  EXPECT_FALSE(get.success);
  EXPECT_FALSE(get.checked_out_by.has_value());

  DelResult del;
  EXPECT_FALSE(del.success);

  LookupResult lup;
  EXPECT_FALSE(lup.found);
  EXPECT_EQ(lup.dbid, kInvalidDBID);
}

TEST(DatabaseTypes, CheckoutInfoFields) {
  CheckoutInfo info;
  info.base_addr = Address(0x7F000001, 7100);
  info.app_id = 42;
  info.entity_id = 1001;

  EXPECT_EQ(info.base_addr.Port(), 7100u);
  EXPECT_EQ(info.app_id, 42u);
  EXPECT_EQ(info.entity_id, 1001u);
}

TEST(DatabaseTypes, CreateDatabaseRejectsRemovedSqlite) {
  DatabaseConfig cfg;
  cfg.type = "sqlite";

  auto db = CreateDatabase(cfg);
  EXPECT_EQ(db, nullptr);
}

TEST(DatabaseTypes, DatabaseConfigDefaultsToMysql) {
  DatabaseConfig cfg;
  EXPECT_EQ(cfg.type, "mysql");
  EXPECT_FALSE(cfg.mysql_initialize);
}

#if ATLAS_TEST_MYSQL_ENABLED
TEST(DatabaseTypes, MysqlPluginExportsOnlyTheCurrentDatabaseInterface) {
  auto executable = fs::ExecutablePath();
  ASSERT_TRUE(executable.HasValue());
#if ATLAS_PLATFORM_WINDOWS
  const auto path = executable->parent_path() / "atlas_db_mysql.dll";
#else
  const auto path = executable->parent_path() / "libatlas_db_mysql.so";
#endif
  auto library = DynamicLibrary::Load(path);
  ASSERT_TRUE(library.HasValue());
  EXPECT_NE(CreateDatabase(DatabaseConfig{}), nullptr);
  using CreateFn = IDatabase* (*)();
  EXPECT_TRUE(library->GetSymbol<CreateFn>("AtlasCreateDatabaseV7").HasValue());
  EXPECT_FALSE(library->GetSymbol<CreateFn>("AtlasCreateDatabaseV6").HasValue());
  EXPECT_FALSE(library->GetSymbol<CreateFn>("AtlasCreateDatabaseV5").HasValue());
  EXPECT_FALSE(library->GetSymbol<CreateFn>("AtlasCreateDatabaseV4").HasValue());
  EXPECT_FALSE(library->GetSymbol<CreateFn>("AtlasCreateDatabaseV3").HasValue());
  EXPECT_FALSE(library->GetSymbol<CreateFn>("AtlasCreateDatabaseV2").HasValue());
  EXPECT_FALSE(library->GetSymbol<CreateFn>("AtlasCreateDatabase").HasValue());
}

#endif

TEST(DatabaseTypes, CreateDatabaseRejectsRemovedXmlBackend) {
  DatabaseConfig cfg;
  cfg.type = "xml";
  EXPECT_EQ(CreateDatabase(cfg), nullptr);
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
