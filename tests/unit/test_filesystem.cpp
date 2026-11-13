#include <chrono>
#include <filesystem>
#include <format>
#include <memory>
#include <string>

#include <gtest/gtest.h>

#include "platform/filesystem.h"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace fs = atlas::fs;

TEST(Filesystem, ReadWriteTextFileRoundTrip) {
  auto tmp = fs::TempDirectory() / "atlas_test_rw.txt";
  std::string content = "Hello, Atlas!\nLine 2\n";

  auto wr = fs::WriteTextFile(tmp, content);
  (void)wr;
  ASSERT_TRUE(wr.HasValue());

  auto rd = fs::ReadTextFile(tmp);
  ASSERT_TRUE(rd.HasValue());
  EXPECT_EQ(*rd, content);

  (void)fs::RemoveFile(tmp);
}

TEST(Filesystem, ExistsAfterWrite) {
  auto tmp = fs::TempDirectory() / "atlas_test_exists.txt";
  (void)fs::WriteTextFile(tmp, "data");
  EXPECT_TRUE(fs::Exists(tmp));
  (void)fs::RemoveFile(tmp);
  EXPECT_FALSE(fs::Exists(tmp));
}

TEST(Filesystem, AtomicReplaceFileReplacesTarget) {
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  auto target = fs::TempDirectory() / std::format("atlas_test_atomic_{}.txt", stamp);
  auto source = target;
  source += ".tmp";

  ASSERT_TRUE(fs::WriteTextFile(target, "old").HasValue());
  ASSERT_TRUE(fs::WriteTextFile(source, "new").HasValue());
  auto replace = fs::AtomicReplaceFile(source, target);
  ASSERT_TRUE(replace.HasValue()) << replace.Error().Message();

  auto content = fs::ReadTextFile(target);
  ASSERT_TRUE(content.HasValue());
  EXPECT_EQ(*content, "new");
  EXPECT_FALSE(fs::Exists(source));

  (void)fs::RemoveFile(source);
  (void)fs::RemoveFile(target);
}

TEST(Filesystem, ScopedFileLockRejectsSecondHolderAndAllowsReacquire) {
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  auto path = fs::TempDirectory() / std::format("atlas_test_lock_{}.lock", stamp);

  {
    auto first = fs::ScopedFileLock::TryAcquire(path, "first");
    ASSERT_TRUE(first.HasValue()) << first.Error().Message();
    EXPECT_TRUE(first->IsHeld());

    auto second = fs::ScopedFileLock::TryAcquire(path, "second");
    ASSERT_FALSE(second.HasValue());
    EXPECT_EQ(second.Error().Code(), atlas::ErrorCode::kAlreadyExists);
  }

  {
    auto reacquired = fs::ScopedFileLock::TryAcquire(path, "third");
    ASSERT_TRUE(reacquired.HasValue()) << reacquired.Error().Message();
    EXPECT_TRUE(reacquired->IsHeld());
  }

  (void)fs::RemoveFile(path);
}

TEST(Filesystem, AtomicReplaceCreatesMissingTarget) {
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  auto target = fs::TempDirectory() / std::format("atlas_atomic_new_{}.txt", stamp);
  auto source = target;
  source += ".tmp";
  ASSERT_TRUE(fs::WriteTextFile(source, "new"));
  ASSERT_TRUE(fs::AtomicReplaceFile(source, target));
  auto content = fs::ReadTextFile(target);
  ASSERT_TRUE(content);
  EXPECT_EQ(*content, "new");
  EXPECT_FALSE(fs::Exists(source));
  (void)fs::RemoveFile(target);
}

TEST(Filesystem, AtomicReplaceMissingSourcePreservesTarget) {
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  auto target = fs::TempDirectory() / std::format("atlas_atomic_missing_{}.txt", stamp);
  auto source = target;
  source += ".tmp";
  ASSERT_TRUE(fs::WriteTextFile(target, "old"));
  auto result = fs::AtomicReplaceFile(source, target);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.Error().Code(), atlas::ErrorCode::kIoError);
  auto content = fs::ReadTextFile(target);
  ASSERT_TRUE(content);
  EXPECT_EQ(*content, "old");
  EXPECT_FALSE(fs::Exists(source));
  (void)fs::RemoveFile(target);
}

TEST(Filesystem, AtomicReplaceRejectsEmptyPaths) {
  const auto result = fs::AtomicReplaceFile({}, "target");
  ASSERT_FALSE(result);
  EXPECT_EQ(result.Error().Code(), atlas::ErrorCode::kInvalidArgument);
  const auto missing_target = fs::AtomicReplaceFile("source", {});
  ASSERT_FALSE(missing_target);
  EXPECT_EQ(missing_target.Error().Code(), atlas::ErrorCode::kInvalidArgument);
}

#if defined(_WIN32)
TEST(Filesystem, AtomicReplaceAllowsDeleteSharedTargetReader) {
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  auto target = fs::TempDirectory() / std::format("atlas_atomic_reader_{}.txt", stamp);
  auto source = target;
  source += ".tmp";
  ASSERT_TRUE(fs::WriteTextFile(target, "old"));
  ASSERT_TRUE(fs::WriteTextFile(source, "new"));
  {
    std::unique_ptr<void, decltype(&::CloseHandle)> reader(
        ::CreateFileW(target.c_str(), GENERIC_READ,
                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr),
        ::CloseHandle);
    ASSERT_NE(reader.get(), INVALID_HANDLE_VALUE);
    WIN32_FILE_ATTRIBUTE_DATA before{};
    ASSERT_TRUE(::GetFileAttributesExW(target.c_str(), GetFileExInfoStandard, &before));
    auto result = fs::AtomicReplaceFile(source, target);
    ASSERT_TRUE(result) << result.Error().Message();
    char previous[3]{};
    DWORD read = 0;
    ASSERT_TRUE(::ReadFile(reader.get(), previous, sizeof(previous), &read, nullptr));
    EXPECT_EQ(read, 3u);
    EXPECT_EQ(std::string(previous, read), "old");
    auto content = fs::ReadTextFile(target);
    ASSERT_TRUE(content);
    EXPECT_EQ(*content, "new");
    EXPECT_FALSE(fs::Exists(source));
    WIN32_FILE_ATTRIBUTE_DATA after{};
    ASSERT_TRUE(::GetFileAttributesExW(target.c_str(), GetFileExInfoStandard, &after));
    EXPECT_EQ(after.ftCreationTime.dwLowDateTime, before.ftCreationTime.dwLowDateTime);
    EXPECT_EQ(after.ftCreationTime.dwHighDateTime, before.ftCreationTime.dwHighDateTime);
  }
  (void)fs::RemoveFile(target);
}

TEST(Filesystem, AtomicReplaceRejectsBlockedTargetAndOpenSource) {
  for (bool hold_source : {false, true}) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    auto target = fs::TempDirectory() / std::format("atlas_atomic_blocked_{}.txt", stamp);
    auto source = target;
    source += ".tmp";
    ASSERT_TRUE(fs::WriteTextFile(target, "old"));
    ASSERT_TRUE(fs::WriteTextFile(source, "new"));
    {
      const auto& held = hold_source ? source : target;
      const DWORD sharing =
          FILE_SHARE_READ | FILE_SHARE_WRITE | (hold_source ? FILE_SHARE_DELETE : 0);
      std::unique_ptr<void, decltype(&::CloseHandle)> reader(
          ::CreateFileW(held.c_str(), GENERIC_READ, sharing, nullptr, OPEN_EXISTING,
                        FILE_ATTRIBUTE_NORMAL, nullptr),
          ::CloseHandle);
      ASSERT_NE(reader.get(), INVALID_HANDLE_VALUE);
      auto result = fs::AtomicReplaceFile(source, target);
      ASSERT_FALSE(result);
      EXPECT_EQ(result.Error().Code(), atlas::ErrorCode::kIoError);
      auto old_content = fs::ReadTextFile(target);
      auto new_content = fs::ReadTextFile(source);
      ASSERT_TRUE(old_content);
      ASSERT_TRUE(new_content);
      EXPECT_EQ(*old_content, "old");
      EXPECT_EQ(*new_content, "new");
    }
    (void)fs::RemoveFile(source);
    (void)fs::RemoveFile(target);
  }
}
#endif

TEST(Filesystem, FileSizeCorrect) {
  auto tmp = fs::TempDirectory() / "atlas_test_size.txt";
  std::string data = "12345";
  (void)fs::WriteTextFile(tmp, data);

  auto sz = fs::FileSize(tmp);
  ASSERT_TRUE(sz.HasValue());
  EXPECT_EQ(*sz, static_cast<std::uintmax_t>(data.size()));

  (void)fs::RemoveFile(tmp);
}

TEST(Filesystem, ReadNonexistentReturnsError) {
  auto result = fs::ReadFile(fs::TempDirectory() / "atlas_nonexistent_file_42.bin");
  EXPECT_FALSE(result.HasValue());
}

TEST(Filesystem, CreateDirectoriesNested) {
  auto dir = fs::TempDirectory() / "atlas_test_dirs" / "a" / "b" / "c";
  auto r = fs::CreateDirectories(dir);
  (void)r;
  ASSERT_TRUE(r.HasValue());
  EXPECT_TRUE(fs::Exists(dir));

  std::filesystem::remove_all(fs::TempDirectory() / "atlas_test_dirs");
}

TEST(Filesystem, ExecutablePathNonEmpty) {
  auto ep = fs::ExecutablePath();
  ASSERT_TRUE(ep.HasValue());
  EXPECT_FALSE(ep->empty());
}
