#ifndef ATLAS_LIB_PLATFORM_ENVIRONMENT_H_
#define ATLAS_LIB_PLATFORM_ENVIRONMENT_H_

#include <string>

#include "foundation/error.h"

namespace atlas {

// Environment variable names must be ASCII; values are returned as UTF-8.
[[nodiscard]] auto ReadEnvironmentVariable(const char* name) -> Result<std::string>;

}  // namespace atlas

#endif  // ATLAS_LIB_PLATFORM_ENVIRONMENT_H_
