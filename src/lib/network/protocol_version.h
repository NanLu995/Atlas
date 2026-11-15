#ifndef ATLAS_LIB_NETWORK_PROTOCOL_VERSION_H_
#define ATLAS_LIB_NETWORK_PROTOCOL_VERSION_H_

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace atlas {

inline constexpr uint16_t kConnectionProtocolVersion = 1;
inline constexpr std::array<std::byte, 6> kConnectionProtocolHeader{
    std::byte{0x41},
    std::byte{0x54},
    std::byte{0x4c},
    std::byte{0x53},
    static_cast<std::byte>(kConnectionProtocolVersion & 0xff),
    static_cast<std::byte>(kConnectionProtocolVersion >> 8)};

[[nodiscard]] inline auto HasConnectionProtocolHeader(std::span<const std::byte> data) -> bool {
  return data.size() >= kConnectionProtocolHeader.size() &&
         std::equal(kConnectionProtocolHeader.begin(), kConnectionProtocolHeader.end(),
                    data.begin());
}

[[nodiscard]] inline auto WithConnectionProtocolHeader(std::span<const std::byte> payload)
    -> std::vector<std::byte> {
  std::vector<std::byte> data;
  data.reserve(kConnectionProtocolHeader.size() + payload.size());
  data.insert(data.end(), kConnectionProtocolHeader.begin(), kConnectionProtocolHeader.end());
  data.insert(data.end(), payload.begin(), payload.end());
  return data;
}

}  // namespace atlas

#endif  // ATLAS_LIB_NETWORK_PROTOCOL_VERSION_H_
