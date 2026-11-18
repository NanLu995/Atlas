#include "server/id_client.h"

#include "foundation/log.h"

namespace atlas {

auto IDClient::AllocateId() -> EntityID {
  if (total_available_ < kCriticallyLow) {
    // Silent before the first DBApp batch lands: AppInit retries via
    // DeferAppInit, no need to warn on the expected pre-init race.
    if (ever_received_) {
      ATLAS_LOG_WARNING("IDClient: critically low ({} IDs remaining)", total_available_);
    }
    return kInvalidEntityID;
  }

  while (!ranges_.empty()) {
    auto& front = ranges_.front();
    if (front.start <= front.end) {
      EntityID id = front.start++;
      --total_available_;
      if (front.start > front.end) ranges_.pop_front();
      return id;
    }
    ranges_.pop_front();
  }

  return kInvalidEntityID;
}

void IDClient::AddIds(EntityID start, EntityID end) {
  request_deadline_ = {};
  // Discarding a late range wastes IDs; accepting overlapping IDs could reuse live identities.
  if (!IsServerEntityId(start) || !IsServerEntityId(end) || start > end ||
      start <= highest_received_id_) {
    ATLAS_LOG_WARNING("IDClient: ignoring invalid range [{}, {}]", start, end);
    return;
  }

  uint64_t count = static_cast<uint64_t>(end) - start + 1;
  ranges_.push_back({start, end});
  highest_received_id_ = end;
  total_available_ += count;
  ever_received_ = true;
  ATLAS_LOG_DEBUG("IDClient: added range [{}, {}] ({} IDs), total_available={}", start, end, count,
                  total_available_);
}

auto IDClient::NeedsRefill() const -> bool {
  return total_available_ < kLow;
}

auto IDClient::IdsToRequest(TimePoint now) const -> uint32_t {
  if (now < request_deadline_ || total_available_ >= kHigh) return 0;

  return static_cast<uint32_t>(kDesired);
}

}  // namespace atlas
