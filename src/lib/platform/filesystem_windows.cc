#include "platform/filesystem.h"

#include <format>

#include <windows.h>

namespace atlas::fs {

[[nodiscard]] auto AtomicReplaceFile(const stdfs::path& source, const stdfs::path& target)
    -> Result<void> {
  if (source.empty() || target.empty()) {
    return Error{ErrorCode::kInvalidArgument, "AtomicReplaceFile requires source and target"};
  }
  if (::ReplaceFileW(target.c_str(), source.c_str(), nullptr, 0, nullptr, nullptr)) {
    return {};
  }
  DWORD error = ::GetLastError();
  if (error == ERROR_FILE_NOT_FOUND) {
    if (::MoveFileExW(source.c_str(), target.c_str(),
                      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
      return {};
    }
    error = ::GetLastError();
  }
  return Error{ErrorCode::kIoError, std::format("Failed to replace file {} with {}: {}",
                                                target.string(), source.string(), error)};
}

}  // namespace atlas::fs
