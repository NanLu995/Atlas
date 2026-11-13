#include <cstdlib>

#include "platform/environment.h"

namespace atlas {

auto ReadEnvironmentVariable(const char* name) -> Result<std::string> {
  const char* value = std::getenv(name);
  return value ? std::string(value) : std::string{};
}

}  // namespace atlas
