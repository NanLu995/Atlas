#include <string>

#include <gtest/gtest.h>

#include "platform/sha256.h"

TEST(Sha256Test, DigestComparisonCoversEveryByte) {
  atlas::Sha256Digest expected{};
  EXPECT_TRUE(atlas::Sha256DigestEquals(expected, expected));
  for (std::size_t index = 0; index < expected.size(); ++index) {
    auto changed = expected;
    changed[index] = std::byte{1};
    EXPECT_FALSE(atlas::Sha256DigestEquals(expected, changed));
  }
}

TEST(Sha256Test, EmptyAndKnownTextMatchStandardDigests) {
  const std::pair<std::string, std::string> cases[] = {
      {"", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
      {"abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
      {std::string(1000000, 'a'),
       "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"}};
  for (const auto& [input, expected] : cases) {
    const auto bytes = input.empty() ? std::span<const std::byte>{}
                                     : std::as_bytes(std::span(input.data(), input.size()));
    auto digest = atlas::Sha256(bytes);
    ASSERT_TRUE(digest.HasValue());
    std::string hex;
    constexpr char kHex[] = "0123456789abcdef";
    for (auto byte : *digest) {
      const auto value = std::to_integer<unsigned char>(byte);
      hex += kHex[value >> 4];
      hex += kHex[value & 15];
    }
    EXPECT_EQ(hex, expected);
  }
}
