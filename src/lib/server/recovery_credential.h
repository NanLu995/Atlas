#ifndef ATLAS_LIB_SERVER_RECOVERY_CREDENTIAL_H_
#define ATLAS_LIB_SERVER_RECOVERY_CREDENTIAL_H_

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

#include "serialization/binary_stream.h"
#include "server/entity_types.h"

namespace atlas {

struct RecoveryCredential {
  DatabaseID account_dbid{kInvalidDBID};
  uint16_t account_type_id{0};
  std::array<std::byte, 32> value{};
  int64_t expires_at_ms{0};

  [[nodiscard]] auto IsPresent() const -> bool {
    return std::ranges::any_of(value, [](std::byte byte) { return byte != std::byte{0}; });
  }

  [[nodiscard]] auto operator==(const RecoveryCredential&) const -> bool = default;
};

inline void WriteRecoveryCredential(BinaryWriter& writer, const RecoveryCredential& credential) {
  writer.Write(static_cast<uint8_t>(credential.IsPresent()));
  if (!credential.IsPresent()) return;
  writer.Write(credential.account_dbid);
  writer.Write(credential.account_type_id);
  writer.WriteBytes(credential.value);
  writer.Write(credential.expires_at_ms);
}

[[nodiscard]] inline auto ReadRecoveryCredential(BinaryReader& reader)
    -> Result<RecoveryCredential> {
  auto present = reader.Read<uint8_t>();
  if (!present || *present > 1)
    return Error{ErrorCode::kInvalidArgument, "invalid recovery credential flag"};
  if (*present == 0) return RecoveryCredential{};
  auto dbid = reader.Read<DatabaseID>();
  auto type = reader.Read<uint16_t>();
  auto value = reader.ReadBytes(32);
  auto expires = reader.Read<int64_t>();
  if (!dbid || *dbid <= kInvalidDBID || !type || *type == 0 || !value || !expires || *expires <= 0)
    return Error{ErrorCode::kInvalidArgument, "invalid or truncated recovery credential"};
  RecoveryCredential result{*dbid, *type, {}, *expires};
  std::ranges::copy(*value, result.value.begin());
  if (!result.IsPresent()) return Error{ErrorCode::kInvalidArgument, "empty recovery credential"};
  return result;
}

}  // namespace atlas

#endif  // ATLAS_LIB_SERVER_RECOVERY_CREDENTIAL_H_
