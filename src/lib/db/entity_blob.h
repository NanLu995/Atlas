#ifndef ATLAS_LIB_DB_ENTITY_BLOB_H_
#define ATLAS_LIB_DB_ENTITY_BLOB_H_

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

#include "platform/sha256.h"
#include "serialization/binary_stream.h"

namespace atlas {

inline constexpr uint8_t kDbBlobMagic = 0xa7;
inline constexpr uint8_t kDbBlobVersion = 2;

struct DbBlobSections {
  std::span<const std::byte> base;
  std::span<const std::byte> cell;
};

[[nodiscard]] inline auto EncodeDbBlob(std::span<const std::byte> base,
                                       std::span<const std::byte> cell)
    -> Result<std::vector<std::byte>> {
  if (base.size() > std::numeric_limits<uint32_t>::max() ||
      cell.size() > std::numeric_limits<uint32_t>::max())
    return Error{ErrorCode::kMessageTooLarge, "entity snapshot section exceeds uint32 length"};
  BinaryWriter writer;
  writer.Write(kDbBlobMagic);
  writer.Write(kDbBlobVersion);
  writer.Write(static_cast<uint32_t>(base.size()));
  writer.WriteBytes(base);
  writer.Write(static_cast<uint32_t>(cell.size()));
  writer.WriteBytes(cell);
  auto checksum = Sha256(writer.Data());
  if (!checksum) return checksum.Error();
  writer.WriteBytes(*checksum);
  return writer.Detach();
}

[[nodiscard]] inline auto DecodeDbBlob(std::span<const std::byte> blob) -> Result<DbBlobSections> {
  constexpr std::size_t kChecksumSize = Sha256Digest{}.size();
  if (blob.size() < 2 + 2 * sizeof(uint32_t) + kChecksumSize)
    return Error{ErrorCode::kInvalidArgument, "entity snapshot is truncated or unversioned"};
  const auto payload = blob.first(blob.size() - kChecksumSize);
  BinaryReader reader(payload);
  const auto magic = reader.Read<uint8_t>();
  const auto version = reader.Read<uint8_t>();
  if (!magic || !version || *magic != kDbBlobMagic || *version != kDbBlobVersion)
    return Error{ErrorCode::kInvalidArgument, "unsupported entity snapshot magic or version"};
  const auto base_size = reader.Read<uint32_t>();
  if (!base_size) return base_size.Error();
  auto base = reader.ReadBytes(*base_size);
  if (!base) return Error{ErrorCode::kInvalidArgument, "entity snapshot base section is truncated"};
  const auto cell_size = reader.Read<uint32_t>();
  if (!cell_size) return cell_size.Error();
  auto cell = reader.ReadBytes(*cell_size);
  if (!cell) return Error{ErrorCode::kInvalidArgument, "entity snapshot cell section is truncated"};
  if (reader.Remaining() != 0)
    return Error{ErrorCode::kInvalidArgument, "entity snapshot has trailing data"};
  auto checksum = Sha256(payload);
  if (!checksum) return checksum.Error();
  if (!std::equal(checksum->begin(), checksum->end(), blob.end() - kChecksumSize))
    return Error{ErrorCode::kInvalidArgument, "entity snapshot checksum mismatch"};
  return DbBlobSections{*base, *cell};
}

}  // namespace atlas

#endif  // ATLAS_LIB_DB_ENTITY_BLOB_H_
