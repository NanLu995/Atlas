#include <sys/random.h>

#include <cerrno>

#include "platform/secure_random.h"

namespace atlas {

auto FillSecureRandom(std::span<std::byte> output) -> Result<void> {
  while (!output.empty()) {
    const auto count = getrandom(output.data(), output.size(), 0);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) return Error{ErrorCode::kIoError, "getrandom failed"};
    output = output.subspan(static_cast<size_t>(count));
  }
  return {};
}

}  // namespace atlas
