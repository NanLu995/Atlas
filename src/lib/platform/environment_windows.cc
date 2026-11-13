#include <windows.h>

#include <cstring>
#include <vector>

#include "platform/environment.h"

namespace atlas {

auto ReadEnvironmentVariable(const char* name) -> Result<std::string> {
  const std::wstring key(name, name + std::strlen(name));
  SetLastError(ERROR_SUCCESS);
  const DWORD length = GetEnvironmentVariableW(key.c_str(), nullptr, 0);
  if (length == 0) {
    const auto error = GetLastError();
    if (error == ERROR_ENVVAR_NOT_FOUND || error == ERROR_SUCCESS) return std::string{};
    return Error{ErrorCode::kIoError, "cannot read environment variable"};
  }
  std::vector<wchar_t> buffer(length);
  const DWORD written = GetEnvironmentVariableW(key.c_str(), buffer.data(), length);
  if (written == 0 || written >= length)
    return Error{ErrorCode::kIoError, "environment changed while reading variable"};
  const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, buffer.data(),
                                       static_cast<int>(written), nullptr, 0, nullptr, nullptr);
  if (size <= 0) return Error{ErrorCode::kIoError, "invalid environment variable encoding"};
  std::string value(static_cast<std::size_t>(size), '\0');
  if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, buffer.data(), static_cast<int>(written),
                          value.data(), size, nullptr, nullptr) != size)
    return Error{ErrorCode::kIoError, "cannot decode environment variable"};
  return value;
}

}  // namespace atlas
