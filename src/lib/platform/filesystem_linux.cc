#include "platform/filesystem.h"

#include <format>

namespace atlas::fs {

[[nodiscard]] auto AtomicReplaceFile(const stdfs::path& source, const stdfs::path& target)
    -> Result<void> {
  if (source.empty() || target.empty()) {
    return Error{ErrorCode::kInvalidArgument, "AtomicReplaceFile requires source and target"};
  }
  std::error_code ec;
  stdfs::rename(source, target, ec);
  if (ec) {
    return Error{ErrorCode::kIoError,
                 std::format("Failed to replace file {} with {}: {}", target.string(),
                             source.string(), ec.message())};
  }
  return {};
}

}  // namespace atlas::fs
