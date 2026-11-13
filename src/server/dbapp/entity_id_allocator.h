#ifndef ATLAS_SERVER_DBAPP_ENTITY_ID_ALLOCATOR_H_
#define ATLAS_SERVER_DBAPP_ENTITY_ID_ALLOCATOR_H_

#include <cstdint>
#include <functional>

#include "db/idatabase.h"
#include "server/entity_types.h"

namespace atlas {

class EntityIdAllocator {
 public:
  explicit EntityIdAllocator(IDatabase& db);

  void Allocate(uint32_t count, std::function<void(Result<EntityIdRange>)> callback);

 private:
  IDatabase& db_;
};

}  // namespace atlas

#endif  // ATLAS_SERVER_DBAPP_ENTITY_ID_ALLOCATOR_H_
