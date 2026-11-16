#include "platform/password_hash.h"

#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/params.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <span>

#include "platform/secure_random.h"

namespace atlas {
namespace {

constexpr std::string_view kPrefix = "$argon2id$v=19$m=65536,t=3,p=4$";
using Salt = std::array<unsigned char, 16>;
using Derived = std::array<unsigned char, 32>;

auto Encode(std::span<const unsigned char> bytes) -> std::string {
  std::string output(4 * ((bytes.size() + 2) / 3) + 1, '\0');
  const auto length = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(output.data()), bytes.data(),
                                      static_cast<int>(bytes.size()));
  output.resize(static_cast<size_t>(length));
  while (output.back() == '=') output.pop_back();
  return output;
}

template <size_t N>
auto Decode(std::string_view input, std::array<unsigned char, N>& output) -> bool {
  if (input.size() != (N * 8 + 5) / 6 ||
      input.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/") !=
          std::string_view::npos) {
    return false;
  }
  std::string padded(input);
  padded.append((4 - padded.size() % 4) % 4, '=');
  std::array<unsigned char, N + 3> decoded{};
  const auto length =
      EVP_DecodeBlock(decoded.data(), reinterpret_cast<const unsigned char*>(padded.data()),
                      static_cast<int>(padded.size()));
  if (length < static_cast<int>(N)) return false;
  std::copy_n(decoded.begin(), N, output.begin());
  return Encode(output) == input;
}

auto Derive(std::string_view password, const Salt& salt) -> Result<Derived> {
  if (password.empty() || password.find('\0') != std::string_view::npos) {
    return Error{ErrorCode::kInvalidArgument, "Password must be nonempty and contain no NUL"};
  }
  std::unique_ptr<EVP_KDF, decltype(&EVP_KDF_free)> kdf(EVP_KDF_fetch(nullptr, "ARGON2ID", nullptr),
                                                        EVP_KDF_free);
  if (!kdf) return Error{ErrorCode::kInternalError, "Argon2id provider unavailable"};
  std::unique_ptr<EVP_KDF_CTX, decltype(&EVP_KDF_CTX_free)> context(EVP_KDF_CTX_new(kdf.get()),
                                                                    EVP_KDF_CTX_free);
  if (!context) return Error{ErrorCode::kOutOfMemory, "Argon2id context allocation failed"};
  uint32_t memory = 65536;
  uint32_t iterations = 3;
  uint32_t lanes = 4;
  uint32_t threads = 1;
  uint32_t version = 0x13;
  const OSSL_PARAM params[] = {
      OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_PASSWORD, const_cast<char*>(password.data()),
                                        password.size()),
      OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_SALT,
                                        const_cast<unsigned char*>(salt.data()), salt.size()),
      OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_ARGON2_MEMCOST, &memory),
      OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_ITER, &iterations),
      OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_ARGON2_LANES, &lanes),
      OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_THREADS, &threads),
      OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_ARGON2_VERSION, &version),
      OSSL_PARAM_construct_end()};
  Derived output{};
  if (EVP_KDF_derive(context.get(), output.data(), output.size(), params) != 1) {
    OPENSSL_cleanse(output.data(), output.size());
    return Error{ErrorCode::kInternalError, "Argon2id derivation failed"};
  }
  return output;
}

}  // namespace

auto HashPassword(std::string_view password) -> Result<std::string> {
  Salt salt{};
  auto random = FillSecureRandom(std::as_writable_bytes(std::span(salt)));
  if (!random) return random.Error();
  auto derived = Derive(password, salt);
  if (!derived) return derived.Error();
  auto record = std::string(kPrefix) + Encode(salt) + "$" + Encode(*derived);
  OPENSSL_cleanse(derived->data(), derived->size());
  return record;
}

auto VerifyPassword(std::string_view password, std::string_view record) -> Result<bool> {
  if (!record.starts_with(kPrefix) || record.size() != kPrefix.size() + 22 + 1 + 43 ||
      record[kPrefix.size() + 22] != '$') {
    return Error{ErrorCode::kInvalidArgument, "Unsupported password record"};
  }
  Salt salt{};
  Derived expected{};
  if (!Decode(record.substr(kPrefix.size(), 22), salt) ||
      !Decode(record.substr(kPrefix.size() + 23), expected)) {
    return Error{ErrorCode::kInvalidArgument, "Invalid password record encoding"};
  }
  auto derived = Derive(password, salt);
  if (!derived) return derived.Error();
  const bool matches = CRYPTO_memcmp(derived->data(), expected.data(), expected.size()) == 0;
  OPENSSL_cleanse(derived->data(), derived->size());
  OPENSSL_cleanse(expected.data(), expected.size());
  return matches;
}

}  // namespace atlas
