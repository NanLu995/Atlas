#include <openssl/crypto.h>

#include "platform/sha256.h"

namespace atlas {

auto Sha256DigestEquals(const Sha256Digest& left, const Sha256Digest& right) -> bool {
  return CRYPTO_memcmp(left.data(), right.data(), left.size()) == 0;
}

}  // namespace atlas
