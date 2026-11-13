#include <limits>

#include "platform/secure_random.h"

// bcrypt needs the Windows type definitions before its declarations.
// clang-format off
#include <windows.h>
#include <bcrypt.h>
// clang-format on

namespace atlas {

auto FillSecureRandom(std::span<std::byte> output) -> Result<void> {
  if (output.empty()) return {};
  if (output.size() > (std::numeric_limits<ULONG>::max)()) {
    return Error{ErrorCode::kOutOfRange, "secure random request exceeds ULONG range"};
  }
  const auto status =
      BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(output.data()),
                      static_cast<ULONG>(output.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
  if (status < 0) return Error{ErrorCode::kIoError, "BCryptGenRandom failed"};
  return {};
}

}  // namespace atlas
