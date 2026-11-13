#ifndef ATLAS_LIB_PLATFORM_SECURE_RANDOM_H_
#define ATLAS_LIB_PLATFORM_SECURE_RANDOM_H_

#include <cstddef>
#include <span>

#include "foundation/error.h"

namespace atlas {

[[nodiscard]] auto FillSecureRandom(std::span<std::byte> output) -> Result<void>;

}  // namespace atlas

#endif  // ATLAS_LIB_PLATFORM_SECURE_RANDOM_H_
