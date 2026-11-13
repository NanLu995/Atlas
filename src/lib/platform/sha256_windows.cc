#include <limits>

#include "platform/sha256.h"

// bcrypt requires Windows type definitions before its declarations.
// clang-format off
#include <windows.h>
#include <bcrypt.h>
// clang-format on

namespace atlas {

auto Sha256(std::span<const std::byte> data) -> Result<Sha256Digest> {
  if (data.size() > (std::numeric_limits<ULONG>::max)()) {
    return Error{ErrorCode::kOutOfRange, "SHA-256 input exceeds ULONG range"};
  }
  Sha256Digest digest{};
  std::byte empty{};
  auto* input = data.empty() ? &empty : const_cast<std::byte*>(data.data());
  const auto status =
      BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, reinterpret_cast<PUCHAR>(input),
                 static_cast<ULONG>(data.size()), reinterpret_cast<PUCHAR>(digest.data()),
                 static_cast<ULONG>(digest.size()));
  if (status < 0) return Error{ErrorCode::kIoError, "BCrypt SHA-256 failed"};
  return digest;
}

}  // namespace atlas
