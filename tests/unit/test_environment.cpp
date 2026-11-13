#include <gtest/gtest.h>

#include "platform/environment.h"

TEST(Environment, ReadsUtf8ValueAndMissingVariable) {
  auto value = atlas::ReadEnvironmentVariable("ATLAS_ENVIRONMENT_TEST_VALUE");
  ASSERT_TRUE(value.HasValue());
  EXPECT_EQ(*value, "Atlas-环境");
  auto missing = atlas::ReadEnvironmentVariable("ATLAS_ENVIRONMENT_TEST_ABSENT_73C921");
  ASSERT_TRUE(missing.HasValue());
  EXPECT_TRUE(missing->empty());
}
