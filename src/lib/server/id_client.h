#ifndef ATLAS_LIB_SERVER_ID_CLIENT_H_
#define ATLAS_LIB_SERVER_ID_CLIENT_H_

#include <cstdint>
#include <deque>

#include "server/entity_types.h"

namespace atlas {

// Caches committed DBApp reservations on the entity app's main loop.
class IDClient {
 public:
  static constexpr uint64_t kCriticallyLow = 16;
  static constexpr uint64_t kLow = 1024;
  static constexpr uint64_t kDesired = 4096;
  static constexpr uint64_t kHigh = 16384;

  IDClient() = default;

  // Returns kInvalidEntityID if the cache is critically low or empty.
  [[nodiscard]] auto AllocateId() -> EntityID;

  void AddIds(EntityID start, EntityID end);

  [[nodiscard]] auto NeedsRefill() const -> bool;

  // 0 if above the high watermark.
  [[nodiscard]] auto IdsToRequest() const -> uint32_t;

  [[nodiscard]] auto Available() const -> uint64_t { return total_available_; }
  [[nodiscard]] auto IsCriticallyLow() const -> bool { return total_available_ < kCriticallyLow; }
  // False until the first AddIds; lets callers distinguish startup wait from
  // true exhaustion and suppress noisy logs during the pre-DBApp-roundtrip window.
  [[nodiscard]] auto HasReceivedAny() const -> bool { return ever_received_; }

 private:
  struct Range {
    EntityID start;
    EntityID end;
  };

  std::deque<Range> ranges_;
  uint64_t total_available_{0};
  EntityID highest_received_id_{0};
  bool ever_received_{false};
};

}  // namespace atlas

#endif  // ATLAS_LIB_SERVER_ID_CLIENT_H_
