#include <openssl/evp.h>

#include "platform/sha256.h"

namespace atlas {

auto Sha256(std::span<const std::byte> data) -> Result<Sha256Digest> {
  Sha256Digest digest{};
  size_t size = 0;
  if (EVP_Q_digest(nullptr, "SHA256", nullptr, data.data(), data.size(),
                   reinterpret_cast<unsigned char*>(digest.data()), &size) != 1 ||
      size != digest.size()) {
    return Error{ErrorCode::kIoError, "OpenSSL SHA-256 failed"};
  }
  return digest;
}

}  // namespace atlas
