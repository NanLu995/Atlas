#ifndef ATLAS_LIB_PLATFORM_PROCESS_LAUNCHER_H_
#define ATLAS_LIB_PLATFORM_PROCESS_LAUNCHER_H_

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "foundation/error.h"

namespace atlas {

struct ProcessLaunchOptions {
  std::filesystem::path exe;
  std::vector<std::string> args;
  std::filesystem::path working_directory;
  std::filesystem::path output_path;
};

// Pins process identity through a Windows handle or Linux pidfd.
// Unknown liveness is treated as alive, so callers cannot mistake it for confirmed exit.
class LaunchedProcess {
 public:
  LaunchedProcess() = default;
  LaunchedProcess(const LaunchedProcess&) = delete;
  auto operator=(const LaunchedProcess&) -> LaunchedProcess& = delete;
  LaunchedProcess(LaunchedProcess&& other) noexcept;
  auto operator=(LaunchedProcess&& other) noexcept -> LaunchedProcess&;
  ~LaunchedProcess();

  [[nodiscard]] auto Pid() const -> uint32_t { return pid_; }
  [[nodiscard]] auto IsValid() const -> bool { return pid_ != 0; }
  [[nodiscard]] auto IsAlive() const -> bool;
  auto Terminate() -> bool;
  void Reset();

 private:
  LaunchedProcess(uint32_t pid, uint64_t platform_token) noexcept
      : pid_(pid), platform_token_(platform_token) {}
  friend auto LaunchDetachedProcess(ProcessLaunchOptions opts) -> Result<LaunchedProcess>;
  friend auto ObserveProcess(uint32_t pid) -> Result<LaunchedProcess>;

  uint32_t pid_{0};
  // Windows HANDLE or Linux pidfd + 1; zero means no stable handle.
  uint64_t platform_token_{0};
};

[[nodiscard]] auto LaunchDetachedProcess(ProcessLaunchOptions opts) -> Result<LaunchedProcess>;
[[nodiscard]] auto ObserveProcess(uint32_t pid) -> Result<LaunchedProcess>;

// PID-only fallback: unknown state is alive; use an observed identity before terminating.
[[nodiscard]] auto IsProcessAlive(uint32_t pid) -> bool;

}  // namespace atlas

#endif  // ATLAS_LIB_PLATFORM_PROCESS_LAUNCHER_H_
