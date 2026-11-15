#ifndef ATLAS_TESTS_UNIT_TEST_DATABASE_ADMISSION_H_
#define ATLAS_TESTS_UNIT_TEST_DATABASE_ADMISSION_H_

#include <chrono>
#include <thread>

#include <gtest/gtest.h>

#include "db/idatabase.h"

namespace atlas::test_support {

inline void CheckDatabaseAdmission(IDatabase& database) {
  database.SetDeferredMode(true);
  std::size_t completed = 0;
  for (std::size_t i = 0; i < IDatabase::kMaxPendingOperations; ++i) {
    database.ReadAccountSession(999, 1, [&](Result<AccountSession> result) {
      EXPECT_TRUE(result);
      ++completed;
    });
  }
  EXPECT_EQ(completed, 0u);
  EXPECT_EQ(database.PendingOperations(), IDatabase::kMaxPendingOperations);
  bool write_rejected = false;
  database.PutEntity(kInvalidDBID, 1, WriteFlags::kCreateNew, {}, "admission-rejected",
                     [&](PutResult result) {
                       EXPECT_FALSE(result.success);
                       EXPECT_EQ(result.error_kind, DatabaseErrorKind::kOverloaded);
                       write_rejected = true;
                     });
  EXPECT_TRUE(write_rejected);
  int rejected = 0;
  database.GetAutoLoadEntities([&](Result<std::vector<EntityData>> result) {
    ASSERT_FALSE(result);
    EXPECT_EQ(result.Error().Code(), ErrorCode::kRateLimited);
    ++rejected;
  });
  database.SetAutoLoad(999, 1, true, [&](Result<void> result) {
    ASSERT_FALSE(result);
    EXPECT_EQ(result.Error().Code(), ErrorCode::kRateLimited);
    ++rejected;
  });
  EXPECT_EQ(rejected, 2);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  while (database.PendingOperations() != 0 && std::chrono::steady_clock::now() < deadline) {
    database.ProcessResults();
    std::this_thread::yield();
  }
  EXPECT_EQ(database.PendingOperations(), 0u);
  EXPECT_EQ(completed, IDatabase::kMaxPendingOperations);
  database.SetDeferredMode(false);
  database.LookupByName(1, "admission-rejected", [](LookupResult result) {
    EXPECT_FALSE(result.found);
    EXPECT_TRUE(result.error.empty());
  });
}

inline void CheckDatabaseDrain(IDatabase& database) {
  database.SetDeferredMode(true);
  int completed = 0;
  for (int i = 0; i < 8; ++i) {
    database.ReadAccountSession(999, 1, [&](Result<AccountSession> result) {
      EXPECT_TRUE(result);
      ++completed;
      database.ClearCheckout(999, 1, [](Result<bool> rejected) {
        ASSERT_FALSE(rejected);
        EXPECT_EQ(rejected.Error().Code(), ErrorCode::kReceiverGone);
      });
    });
  }
  database.BeginDrain();
  EXPECT_EQ(database.PendingOperations(), 8u);
  int rejected = 0;
  database.GetAutoLoadEntities([&](Result<std::vector<EntityData>> result) {
    ASSERT_FALSE(result);
    EXPECT_EQ(result.Error().Code(), ErrorCode::kReceiverGone);
    ++rejected;
  });
  database.MarkCheckoutCleared(999, 1, [&](Result<void> result) {
    ASSERT_FALSE(result);
    EXPECT_EQ(result.Error().Code(), ErrorCode::kReceiverGone);
    ++rejected;
  });
  EXPECT_EQ(rejected, 2);
  database.Shutdown();
  EXPECT_EQ(completed, 8);
  EXPECT_EQ(database.PendingOperations(), 0u);
}

}  // namespace atlas::test_support

#endif  // ATLAS_TESTS_UNIT_TEST_DATABASE_ADMISSION_H_
