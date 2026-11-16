#ifndef ATLAS_LIB_PLATFORM_SHA256_H_
#define ATLAS_LIB_PLATFORM_SHA256_H_

#include <array>
#include <cstddef>
#include <span>

#include "foundation/error.h"

namespace atlas {

using Sha256Digest = std::array<std::byte, 32>;

[[nodiscard]] auto Sha256(std::span<const std::byte> data) -> Result<Sha256Digest>;
[[nodiscard]] auto Sha256DigestEquals(const Sha256Digest& left, const Sha256Digest& right) -> bool;

}  // namespace atlas

#endif  // ATLAS_LIB_PLATFORM_SHA256_H_
