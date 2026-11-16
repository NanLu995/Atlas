#ifndef ATLAS_TESTS_UNIT_TEST_BSP_WIRE_H_
#define ATLAS_TESTS_UNIT_TEST_BSP_WIRE_H_

#include <cstdint>
#include <vector>

#include "cellapp/cell_bounds.h"
#include "network/address.h"
#include "serialization/binary_stream.h"

namespace atlas::test_support {

inline void WriteBspLeaf(BinaryWriter& writer, uint32_t id, const Address& owner) {
  writer.Write(uint8_t{0});
  writer.Write(id);
  writer.Write(owner.Ip());
  writer.Write(owner.Port());
}

inline void WriteBalancedBspNodes(BinaryWriter& writer, uint32_t levels, CellBounds bounds,
                                  uint32_t& next_id, const Address& first, const Address& second) {
  if (levels == 0) {
    const auto id = next_id++;
    WriteBspLeaf(writer, id, id % 2 == 0 ? first : second);
    return;
  }
  const bool split_x = levels % 2 == 0;
  const auto midpoint =
      split_x ? (bounds.min_x + bounds.max_x) * 0.5f : (bounds.min_z + bounds.max_z) * 0.5f;
  writer.Write(uint8_t{1});
  writer.Write(static_cast<uint8_t>(split_x ? 0 : 1));
  writer.Write(midpoint);
  auto left = bounds;
  auto right = bounds;
  if (split_x) {
    left.max_x = midpoint;
    right.min_x = midpoint;
  } else {
    left.max_z = midpoint;
    right.min_z = midpoint;
  }
  WriteBalancedBspNodes(writer, levels - 1, left, next_id, first, second);
  WriteBalancedBspNodes(writer, levels - 1, right, next_id, first, second);
}

inline auto BalancedBspBlob(uint32_t levels, const Address& first, const Address& second,
                            uint32_t next_id = 1) -> std::vector<std::byte> {
  BinaryWriter writer;
  const CellBounds bounds{-1000.f, -1000.f, 1000.f, 1000.f};
  bounds.Serialize(writer);
  writer.Write(uint8_t{1});
  WriteBalancedBspNodes(writer, levels, bounds, next_id, first, second);
  return writer.Detach();
}

}  // namespace atlas::test_support

#endif  // ATLAS_TESTS_UNIT_TEST_BSP_WIRE_H_
