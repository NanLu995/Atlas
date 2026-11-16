#include <string>
#include <utility>

#include <gtest/gtest.h>

#include "platform/password_hash.h"

TEST(PasswordHash, RoundTripUsesRandomSaltsAndRejectsWrongPassword) {
  auto first = atlas::HashPassword("test password");
  auto second = atlas::HashPassword("test password");
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  EXPECT_NE(*first, *second);
  EXPECT_TRUE(first->starts_with("$argon2id$v=19$m=65536,t=3,p=4$"));
  auto correct = atlas::VerifyPassword("test password", *first);
  ASSERT_TRUE(correct);
  EXPECT_TRUE(*correct);
  auto wrong = atlas::VerifyPassword("different password", *first);
  ASSERT_TRUE(wrong);
  EXPECT_FALSE(*wrong);
}

TEST(PasswordHash, RejectsEmptyAndEmbeddedNullPasswords) {
  EXPECT_FALSE(atlas::HashPassword(""));
  EXPECT_FALSE(atlas::HashPassword(std::string("pass\0word", 9)));
}

TEST(PasswordHash, MatchesOpenSslCommandLineReference) {
  const std::string record =
      "$argon2id$v=19$m=65536,t=3,p=4$MDEyMzQ1Njc4OWFiY2RlZg$"
      "uKZLaN6muIyoyIYr5waqw3y+zaDbe9aLSPj6Ln/rbz4";
  auto verified = atlas::VerifyPassword("password", record);
  ASSERT_TRUE(verified);
  EXPECT_TRUE(*verified);
}

TEST(PasswordHash, RejectsMalformedAndUnboundedParameterRecords) {
  const std::string valid =
      "$argon2id$v=19$m=65536,t=3,p=4$AAAAAAAAAAAAAAAAAAAAAA$"
      "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
  for (const auto& record : {std::string(), std::string("raw password"), valid.substr(1),
                             valid + "x", valid.substr(0, valid.size() - 1)}) {
    EXPECT_FALSE(atlas::VerifyPassword("password", record));
  }
  for (const auto& [from, to] :
       {std::pair{"m=65536", "m=99999"}, {"v=19", "v=16"}, {"p=4", "p=9"}, {"t=3", "t=9"}}) {
    auto record = valid;
    record.replace(record.find(from), std::string(from).size(), to);
    EXPECT_FALSE(atlas::VerifyPassword("password", record));
  }
  for (char value : {'=', '-', '\0', 'B'}) {
    auto record = valid;
    record.back() = value;
    EXPECT_FALSE(atlas::VerifyPassword("password", record));
  }
}
