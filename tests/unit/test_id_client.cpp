#include <gtest/gtest.h>

#include "server/id_client.h"

using namespace atlas;

TEST(IDClient, AllocateFromSingleRange) {
  IDClient client;
  const auto kProvisioned = 2 * IDClient::kCriticallyLow;
  const auto kSuccessfulAllocs = IDClient::kCriticallyLow + 1;
  client.AddIds(100, 100 + kProvisioned - 1);

  EXPECT_EQ(client.Available(), kProvisioned);
  for (uint64_t i = 0; i < kSuccessfulAllocs; ++i) {
    EXPECT_EQ(client.AllocateId(), 100u + i);
  }
  // Cache now sits at kCriticallyLow - 1; next call refuses.
  EXPECT_EQ(client.Available(), IDClient::kCriticallyLow - 1);
  EXPECT_TRUE(client.IsCriticallyLow());
  EXPECT_EQ(client.AllocateId(), kInvalidEntityID);
}

TEST(IDClient, CriticallyLowRefusesAllocation) {
  IDClient client;
  client.AddIds(1, IDClient::kCriticallyLow - 1);

  // Strictly below kCriticallyLow → allocate_id refuses.
  EXPECT_TRUE(client.IsCriticallyLow());
  EXPECT_EQ(client.AllocateId(), kInvalidEntityID);
}

TEST(IDClient, CriticallyLowThreshold) {
  IDClient client;
  client.AddIds(1, IDClient::kCriticallyLow);

  EXPECT_FALSE(client.IsCriticallyLow());
  EXPECT_NE(client.AllocateId(), kInvalidEntityID);
  EXPECT_EQ(client.Available(), IDClient::kCriticallyLow - 1);

  // Now strictly below kCriticallyLow.
  EXPECT_TRUE(client.IsCriticallyLow());
  EXPECT_EQ(client.AllocateId(), kInvalidEntityID);
}

TEST(IDClient, NeedsRefillBelowLowWatermark) {
  IDClient client;
  EXPECT_TRUE(client.NeedsRefill());  // empty

  client.AddIds(1, IDClient::kLow - 1);
  EXPECT_TRUE(client.NeedsRefill());

  client.AddIds(IDClient::kLow, IDClient::kLow);  // now exactly kLow
  EXPECT_FALSE(client.NeedsRefill());
}

TEST(IDClient, IdsToRequestRespectsHighWatermark) {
  IDClient client;
  client.AddIds(1, IDClient::kHigh);  // at high watermark

  EXPECT_EQ(client.IdsToRequest(), 0u);
}

TEST(IDClient, IdsToRequestReturnsDesiredCount) {
  IDClient client;
  EXPECT_EQ(client.IdsToRequest(), IDClient::kDesired);
}

TEST(IDClient, OutstandingReservationSuppressesRepeatedRequests) {
  IDClient client;
  const auto now = TimePoint{} + std::chrono::seconds(1);
  client.SetRequestPending(true, now);
  for (int tick = 0; tick < 100; ++tick) EXPECT_EQ(client.IdsToRequest(now), 0u);
  client.AddIds(1, IDClient::kDesired);
  for (uint64_t i = 0; i < IDClient::kDesired - IDClient::kLow + 1; ++i) {
    ASSERT_NE(client.AllocateId(), kInvalidEntityID);
  }
  EXPECT_TRUE(client.NeedsRefill());
  EXPECT_EQ(client.IdsToRequest(now), IDClient::kDesired);
}

TEST(IDClient, FailedReservationOrDisconnectedChannelAllowsRetry) {
  IDClient client;
  const auto now = TimePoint{} + std::chrono::seconds(1);
  client.SetRequestPending(true, now);
  client.AddIds(0, 0);
  EXPECT_EQ(client.IdsToRequest(now), IDClient::kDesired);
  client.SetRequestPending(true, now);
  client.SetRequestPending(false, now);
  EXPECT_EQ(client.IdsToRequest(now), IDClient::kDesired);
}

TEST(IDClient, MissingReplyAllowsOneRetryAtEachDeadline) {
  IDClient client;
  const auto start = TimePoint{} + std::chrono::seconds(1);
  client.SetRequestPending(true, start);
  EXPECT_EQ(client.IdsToRequest(start + std::chrono::seconds(5) - std::chrono::nanoseconds(1)), 0u);
  EXPECT_EQ(client.IdsToRequest(start + std::chrono::seconds(5)), IDClient::kDesired);
  client.SetRequestPending(true, start + std::chrono::seconds(5));
  EXPECT_EQ(client.IdsToRequest(start + std::chrono::seconds(10) - std::chrono::nanoseconds(1)),
            0u);
  EXPECT_EQ(client.IdsToRequest(start + std::chrono::seconds(10)), IDClient::kDesired);
}

TEST(IDClient, LateAndOverlappingRepliesCannotReuseIdsAfterRetry) {
  IDClient client;
  const auto start = TimePoint{} + std::chrono::seconds(1);
  client.SetRequestPending(true, start);
  ASSERT_EQ(client.IdsToRequest(start + std::chrono::seconds(5)), IDClient::kDesired);
  client.SetRequestPending(true, start + std::chrono::seconds(5));
  client.AddIds(4097, 8192);
  client.AddIds(1, 4096);
  client.AddIds(4097, 8192);
  client.AddIds(8192, 9000);
  EXPECT_EQ(client.Available(), 4096u);
  EXPECT_EQ(client.AllocateId(), 4097u);
}

TEST(IDClient, MultipleRangesAreConsumedInOrder) {
  IDClient client;
  const auto n = IDClient::kCriticallyLow;  // ≥ 16 IDs per range
  client.AddIds(100, 100 + n - 1);
  client.AddIds(200, 200 + n - 1);
  client.AddIds(300, 300 + n - 1);
  EXPECT_EQ(client.Available(), 3 * n);

  for (uint64_t i = 0; i < n; ++i) EXPECT_EQ(client.AllocateId(), 100u + i);
  for (uint64_t i = 0; i < n; ++i) EXPECT_EQ(client.AllocateId(), 200u + i);
  // Third range only drains while Available stays at-or-above the
  // refusal threshold — AllocateId returns invalid past that point.
  uint64_t third_idx = 0;
  while (!client.IsCriticallyLow()) {
    EXPECT_EQ(client.AllocateId(), 300u + third_idx);
    ++third_idx;
  }
  EXPECT_EQ(client.Available(), IDClient::kCriticallyLow - 1);
  EXPECT_EQ(client.AllocateId(), kInvalidEntityID);
}

TEST(IDClient, EmptyClientHasZeroAvailable) {
  IDClient client;
  EXPECT_EQ(client.Available(), 0u);
  EXPECT_TRUE(client.IsCriticallyLow());
  EXPECT_EQ(client.AllocateId(), kInvalidEntityID);
}

TEST(IDClient, InvalidRangeIsIgnored) {
  IDClient client;
  client.AddIds(10, 5);  // invalid: start > end
  EXPECT_EQ(client.Available(), 0u);

  client.AddIds(kInvalidEntityID, 5);  // invalid: start == 0
  EXPECT_EQ(client.Available(), 0u);
}

TEST(IDClient, DuplicateAndOverlappingReservationsCannotReuseConsumedIds) {
  IDClient client;
  client.AddIds(100, 199);
  for (EntityID id = 100; id < 110; ++id) EXPECT_EQ(client.AllocateId(), id);
  client.AddIds(100, 199);
  client.AddIds(199, 220);
  client.AddIds(50, 99);
  EXPECT_EQ(client.Available(), 90u);
  client.AddIds(300, 399);
  EXPECT_EQ(client.Available(), 190u);
  EXPECT_EQ(client.AllocateId(), 110u);
}

TEST(IDClient, ClientLocalIdsCannotEnterServerCache) {
  IDClient client;
  client.AddIds(kFirstLocalEntityID, kFirstLocalEntityID + 10);
  client.AddIds(kFirstLocalEntityID - 1, kFirstLocalEntityID);
  EXPECT_EQ(client.Available(), 0u);
}
