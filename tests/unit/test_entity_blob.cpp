#include <array>
#include <cstddef>

#include <gtest/gtest.h>

#include "db/entity_blob.h"

namespace atlas {
namespace {

TEST(EntityBlob, EmptyAndBothSectionsRoundTrip) {
  auto empty = EncodeDbBlob({}, {});
  ASSERT_TRUE(empty.HasValue());
  auto decoded_empty = DecodeDbBlob(*empty);
  ASSERT_TRUE(decoded_empty.HasValue());
  EXPECT_TRUE(decoded_empty->base.empty());
  EXPECT_TRUE(decoded_empty->cell.empty());
  const std::array base{std::byte{1}, std::byte{2}};
  const std::array cell{std::byte{3}};
  auto encoded = EncodeDbBlob(base, cell);
  ASSERT_TRUE(encoded.HasValue());
  auto decoded = DecodeDbBlob(*encoded);
  ASSERT_TRUE(decoded.HasValue());
  EXPECT_TRUE(std::equal(base.begin(), base.end(), decoded->base.begin(), decoded->base.end()));
  EXPECT_TRUE(std::equal(cell.begin(), cell.end(), decoded->cell.begin(), decoded->cell.end()));
}

TEST(EntityBlob, EveryTruncatedPrefixIsRejected) {
  const std::array base{std::byte{1}, std::byte{2}};
  auto encoded = EncodeDbBlob(base, base);
  ASSERT_TRUE(encoded.HasValue());
  for (std::size_t size = 0; size < encoded->size(); ++size) {
    SCOPED_TRACE(size);
    EXPECT_FALSE(DecodeDbBlob(std::span(*encoded).first(size)));
  }
}

TEST(EntityBlob, AlteredHeaderPayloadOrChecksumIsRejected) {
  const std::array base{std::byte{1}, std::byte{2}};
  auto encoded = EncodeDbBlob(base, base);
  ASSERT_TRUE(encoded.HasValue());
  for (std::size_t offset = 0; offset < encoded->size(); ++offset) {
    SCOPED_TRACE(offset);
    auto damaged = *encoded;
    damaged[offset] ^= std::byte{0x80};
    EXPECT_FALSE(DecodeDbBlob(damaged));
  }
  encoded->push_back(std::byte{0});
  EXPECT_FALSE(DecodeDbBlob(*encoded));
}

TEST(EntityBlob, UnversionedAndOldVersionBlobsAreRejected) {
  const std::array legacy{std::byte{1}, std::byte{2}, std::byte{3}};
  EXPECT_FALSE(DecodeDbBlob(legacy));
  auto old = EncodeDbBlob({}, {});
  ASSERT_TRUE(old.HasValue());
  (*old)[1] = std::byte{1};
  EXPECT_FALSE(DecodeDbBlob(*old));
}

}  // namespace
}  // namespace atlas
