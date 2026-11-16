#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "process_launcher.h"

namespace atlas {
namespace {

auto WriteAll(int fd, const void* data, std::size_t size) -> bool {
  auto* bytes = static_cast<const char*>(data);
  std::size_t total = 0;
  while (total < size) {
    const ssize_t written = write(fd, bytes + total, size - total);
    if (written < 0 && errno == EINTR) continue;
    if (written <= 0) return false;
    total += static_cast<std::size_t>(written);
  }
  return true;
}

}  // namespace

LaunchedProcess::LaunchedProcess(LaunchedProcess&& other) noexcept
    : pid_(other.pid_), platform_token_(other.platform_token_) {
  other.pid_ = 0;
  other.platform_token_ = 0;
}

auto LaunchedProcess::operator=(LaunchedProcess&& other) noexcept -> LaunchedProcess& {
  if (this != &other) {
    Reset();
    pid_ = other.pid_;
    platform_token_ = other.platform_token_;
    other.pid_ = 0;
    other.platform_token_ = 0;
  }
  return *this;
}

LaunchedProcess::~LaunchedProcess() { Reset(); }

void LaunchedProcess::Reset() {
  if (platform_token_ != 0) close(static_cast<int>(platform_token_ - 1));
  pid_ = 0;
  platform_token_ = 0;
}

auto LaunchedProcess::IsAlive() const -> bool {
  if (pid_ == 0) return false;
  if (platform_token_ == 0) return IsProcessAlive(pid_);
  pollfd descriptor{static_cast<int>(platform_token_ - 1), POLLIN, 0};
  int result;
  do {
    result = poll(&descriptor, 1, 0);
  } while (result < 0 && errno == EINTR);
  if (result < 0) return true;
  return (descriptor.revents & (POLLIN | POLLHUP)) == 0;
}

auto LaunchedProcess::Terminate() -> bool {
  if (platform_token_ == 0) return false;
  if (!IsAlive()) return false;
  return syscall(SYS_pidfd_send_signal, static_cast<int>(platform_token_ - 1), SIGKILL, nullptr,
                 0u) == 0 ||
         errno == ESRCH;
}

auto LaunchDetachedProcess(ProcessLaunchOptions opts) -> Result<LaunchedProcess> {
  if (opts.exe.empty()) {
    return Error{ErrorCode::kInvalidArgument, "LaunchDetachedProcess: exe path empty"};
  }

  int output_fd = -1;
  if (!opts.output_path.empty()) {
    const auto parent = opts.output_path.parent_path();
    if (!parent.empty()) {
      std::error_code ec;
      std::filesystem::create_directories(parent, ec);
      if (ec) {
        return Error{ErrorCode::kInternalError,
                     "create output directory failed: " + ec.message()};
      }
    }
    output_fd = open(opts.output_path.string().c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (output_fd < 0) {
      return Error{ErrorCode::kInternalError,
                   "open output failed: " + std::string(std::strerror(errno))};
    }
  }

  int pipe_fds[2]{};
  if (pipe(pipe_fds) != 0) {
    if (output_fd >= 0) close(output_fd);
    return Error{ErrorCode::kInternalError,
                 "pipe failed: " + std::string(std::strerror(errno))};
  }

  const pid_t pid = fork();
  if (pid < 0) {
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    if (output_fd >= 0) close(output_fd);
    return Error{ErrorCode::kInternalError,
                 "fork failed: " + std::string(std::strerror(errno))};
  }
  if (pid == 0) {
    close(pipe_fds[0]);
    (void)setsid();
    const pid_t child_pid = fork();
    if (child_pid < 0) {
      const pid_t error_pid = -1;
      if (!WriteAll(pipe_fds[1], &error_pid, sizeof(error_pid))) {
        if (output_fd >= 0) close(output_fd);
        _exit(127);
      }
      if (output_fd >= 0) close(output_fd);
      _exit(127);
    }
    if (child_pid > 0) {
      if (!WriteAll(pipe_fds[1], &child_pid, sizeof(child_pid))) {
        if (output_fd >= 0) close(output_fd);
        _exit(127);
      }
      if (output_fd >= 0) close(output_fd);
      _exit(0);
    }
    close(pipe_fds[1]);
    if (output_fd >= 0) {
      if (dup2(output_fd, STDOUT_FILENO) < 0 || dup2(output_fd, STDERR_FILENO) < 0) {
        _exit(127);
      }
      if (output_fd > STDERR_FILENO) close(output_fd);
    }
    if (!opts.working_directory.empty()) {
      if (chdir(opts.working_directory.string().c_str()) != 0) {
        _exit(127);
      }
    }

    std::vector<std::string> storage;
    storage.reserve(opts.args.size() + 1);
    storage.push_back(opts.exe.string());
    for (const auto& arg : opts.args) storage.push_back(arg);

    std::vector<char*> argv;
    argv.reserve(storage.size() + 1);
    for (auto& arg : storage) argv.push_back(arg.data());
    argv.push_back(nullptr);
    execvp(storage[0].c_str(), argv.data());
    _exit(127);
  }

  close(pipe_fds[1]);
  if (output_fd >= 0) close(output_fd);
  pid_t launched_pid = -1;
  auto* out = reinterpret_cast<char*>(&launched_pid);
  std::size_t total = 0;
  while (total < sizeof(launched_pid)) {
    const ssize_t bytes = read(pipe_fds[0], out + total, sizeof(launched_pid) - total);
    if (bytes < 0 && errno == EINTR) continue;
    if (bytes <= 0) break;
    total += static_cast<std::size_t>(bytes);
  }
  close(pipe_fds[0]);

  int status = 0;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
  if (total != sizeof(launched_pid) || launched_pid <= 0) {
    return Error{ErrorCode::kInternalError, "detached launch helper failed"};
  }

  auto observed = ObserveProcess(static_cast<uint32_t>(launched_pid));
  if (observed) return std::move(*observed);
  // An unobserved launch cannot be terminated and blocks replacement while its PID exists.
  return LaunchedProcess(static_cast<uint32_t>(launched_pid), 0);
}

auto ObserveProcess(uint32_t pid) -> Result<LaunchedProcess> {
  if (pid == 0 || pid > static_cast<uint32_t>(std::numeric_limits<pid_t>::max())) {
    return Error{ErrorCode::kInvalidArgument, "invalid process ID"};
  }
  const auto fd = syscall(SYS_pidfd_open, static_cast<pid_t>(pid), 0u);
  if (fd < 0) {
    return Error{errno == ENOSYS ? ErrorCode::kNotSupported : ErrorCode::kIoError,
                 "cannot obtain process pidfd; automatic termination requires pidfd support"};
  }
  return LaunchedProcess(pid, static_cast<uint64_t>(fd) + 1);
}

auto IsProcessAlive(uint32_t pid) -> bool {
  if (pid == 0) return false;
  if (pid > static_cast<uint32_t>(std::numeric_limits<pid_t>::max())) return true;
  if (kill(static_cast<pid_t>(pid), 0) == 0) return true;
  return errno != ESRCH;
}

}  // namespace atlas
