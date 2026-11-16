#ifndef ATLAS_LIB_PLATFORM_PASSWORD_HASH_H_
#define ATLAS_LIB_PLATFORM_PASSWORD_HASH_H_

#include <string>
#include <string_view>

#include "foundation/error.h"

namespace atlas {

// Runs a memory-hard derivation; callers must use a bounded background task queue.
[[nodiscard]] auto HashPassword(std::string_view password) -> Result<std::string>;
[[nodiscard]] auto VerifyPassword(std::string_view password, std::string_view record)
    -> Result<bool>;

}  // namespace atlas

#endif  // ATLAS_LIB_PLATFORM_PASSWORD_HASH_H_
