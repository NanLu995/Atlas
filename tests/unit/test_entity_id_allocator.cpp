#include <functional>
#include <optional>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "db/idatabase.h"
#include "entity_id_allocator.h"

using namespace atlas;

namespace {

class StubDatabase : public IDatabase {
 public:
  EntityID next{1};
  bool fail{false};
  bool deferred{false};
  int calls{0};
  std::optional<EntityIdRange> override_range;
  std::vector<std::function<void()>> pending;

  void ReserveEntityIds(uint32_t count,
                        std::function<void(Result<EntityIdRange>)> callback) override {
    ++calls;
    auto reserve = [this, count, cb = std::move(callback)] {
      if (fail) {
        cb(Error{ErrorCode::kIoError, "reservation failed"});
      } else if (override_range) {
        cb(*override_range);
      } else if (count > kFirstLocalEntityID - next) {
        cb(Error{ErrorCode::kOutOfRange, "IDs exhausted"});
      } else {
        EntityIdRange range{next, next + count - 1};
        next += count;
        cb(range);
      }
    };
    if (deferred)
      pending.push_back(std::move(reserve));
    else
      reserve();
  }

  auto Startup(const DatabaseConfig& /*config*/, const EntityDefRegistry& /*defs*/)
      -> Result<void> override {
    return {};
  }
  void Shutdown() override {}
  void PutEntity(DatabaseID, uint16_t, WriteFlags, std::span<const std::byte>, const std::string&,
                 std::function<void(PutResult)>) override {}
  void GetEntity(DatabaseID, uint16_t, std::function<void(GetResult)>) override {}
  void DelEntity(DatabaseID, uint16_t, std::function<void(DelResult)>) override {}
  void LookupByName(uint16_t, const std::string&, std::function<void(LookupResult)>) override {}
  void CheckoutEntity(DatabaseID, uint16_t, const CheckoutInfo&,
                      std::function<void(GetResult)>) override {}
  void CheckoutEntityByName(uint16_t, const std::string&, const CheckoutInfo&,
                            std::function<void(GetResult)>) override {}
  void ClearCheckout(DatabaseID, uint16_t, std::function<void(bool)>) override {}
  void ClearCheckoutsForAddress(const Address&, std::function<void(int)>) override {}
  void GetAutoLoadEntities(std::function<void(std::vector<EntityData>)>) override {}
  void SetAutoLoad(DatabaseID, uint16_t, bool) override {}
  void GetMaxDbidInRange(DatabaseID, DatabaseID,
                         std::function<void(DbidRangeResult)> callback) override {
    callback(DbidRangeResult{true, kInvalidDBID, {}});
  }
  void ProcessResults() override {}
};

TEST(EntityIdAllocator, ConcurrentReservationsDoNotAllocateOverlappingIds) {
  StubDatabase db;
  db.deferred = true;
  EntityIdAllocator first(db);
  EntityIdAllocator second(db);
  std::vector<EntityIdRange> ranges;
  auto collect = [&](Result<EntityIdRange> range) {
    ASSERT_TRUE(range.HasValue());
    ranges.push_back(*range);
  };
  first.Allocate(10, collect);
  second.Allocate(10, collect);
  EXPECT_TRUE(ranges.empty());
  for (auto& complete : db.pending) complete();
  ASSERT_EQ(ranges.size(), 2u);
  EXPECT_EQ(ranges[0].first, 1u);
  EXPECT_LT(ranges[0].last, ranges[1].first);
  EXPECT_EQ(db.next, 21u);
}

TEST(EntityIdAllocator, InvalidCountDoesNotReachDatabase) {
  StubDatabase db;
  EntityIdAllocator alloc(db);
  for (uint32_t count : {0u, kFirstLocalEntityID, UINT32_MAX}) {
    bool completed = false;
    alloc.Allocate(count, [&](Result<EntityIdRange> range) {
      completed = true;
      ASSERT_FALSE(range.HasValue());
      EXPECT_EQ(range.Error().Code(), ErrorCode::kOutOfRange);
    });
    EXPECT_TRUE(completed);
  }
  EXPECT_EQ(db.calls, 0);
}

TEST(EntityIdAllocator, ReservationFailureDoesNotInventIds) {
  StubDatabase db;
  db.fail = true;
  EntityIdAllocator alloc(db);
  alloc.Allocate(10, [](Result<EntityIdRange> range) {
    ASSERT_FALSE(range.HasValue());
    EXPECT_EQ(range.Error().Code(), ErrorCode::kIoError);
  });
  EXPECT_EQ(db.next, 1u);
  db.fail = false;
  alloc.Allocate(10, [](Result<EntityIdRange> range) {
    ASSERT_TRUE(range.HasValue());
    EXPECT_EQ(range->first, 1u);
    EXPECT_EQ(range->last, 10u);
  });
}

TEST(EntityIdAllocator, InvalidBackendRangesAreRejected) {
  StubDatabase db;
  EntityIdAllocator alloc(db);
  for (auto range : {EntityIdRange{0, 9}, EntityIdRange{10, 1}, EntityIdRange{1, 11},
                     EntityIdRange{kFirstLocalEntityID - 9, kFirstLocalEntityID}}) {
    db.override_range = range;
    alloc.Allocate(10, [](Result<EntityIdRange> result) {
      ASSERT_FALSE(result.HasValue());
      EXPECT_EQ(result.Error().Code(), ErrorCode::kIoError);
    });
  }
}

TEST(EntityIdAllocator, ExhaustionNeverEntersClientLocalRange) {
  StubDatabase db;
  db.next = kFirstLocalEntityID - 5;
  EntityIdAllocator alloc(db);
  alloc.Allocate(5, [](Result<EntityIdRange> range) {
    ASSERT_TRUE(range.HasValue());
    EXPECT_EQ(range->last, kFirstLocalEntityID - 1);
  });
  alloc.Allocate(1, [](Result<EntityIdRange> range) {
    ASSERT_FALSE(range.HasValue());
    EXPECT_EQ(range.Error().Code(), ErrorCode::kOutOfRange);
  });
  EXPECT_EQ(db.next, kFirstLocalEntityID);
}

TEST(EntityIdAllocator, NewAllocatorDoesNotReuseReservedIds) {
  StubDatabase db;
  {
    EntityIdAllocator first(db);
    first.Allocate(100, [](Result<EntityIdRange> range) { ASSERT_TRUE(range.HasValue()); });
  }
  EntityIdAllocator restarted(db);
  restarted.Allocate(1, [](Result<EntityIdRange> range) {
    ASSERT_TRUE(range.HasValue());
    EXPECT_EQ(range->first, 101u);
  });
}

}  // namespace
