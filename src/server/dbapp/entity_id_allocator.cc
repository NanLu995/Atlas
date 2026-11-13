#include "entity_id_allocator.h"

#include <utility>

namespace atlas {

EntityIdAllocator::EntityIdAllocator(IDatabase& db) : db_(db) {}

void EntityIdAllocator::Allocate(uint32_t count,
                                 std::function<void(Result<EntityIdRange>)> callback) {
  if (count == 0 || count >= kFirstLocalEntityID) {
    callback(Error{ErrorCode::kOutOfRange, "invalid EntityID reservation count"});
    return;
  }
  db_.ReserveEntityIds(count, [count, cb = std::move(callback)](Result<EntityIdRange> range) {
    if (range && (!IsServerEntityId(range->first) || !IsServerEntityId(range->last) ||
                  range->last < range->first || range->last - range->first != count - 1)) {
      cb(Error{ErrorCode::kIoError, "database returned an invalid EntityID range"});
      return;
    }
    cb(std::move(range));
  });
}

}  // namespace atlas
