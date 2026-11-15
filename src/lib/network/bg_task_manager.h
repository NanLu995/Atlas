#ifndef ATLAS_LIB_NETWORK_BG_TASK_MANAGER_H_
#define ATLAS_LIB_NETWORK_BG_TASK_MANAGER_H_

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include "foundation/error.h"
#include "network/event_dispatcher.h"
#include "network/frequent_task.h"

namespace atlas {

class EventDispatcher;

class BackgroundTask {
 public:
  virtual ~BackgroundTask() = default;

  // Must not access dispatcher-owned network objects.
  virtual auto DoBackgroundTask() -> Result<void> = 0;

  virtual void DoMainThreadTask(Result<void> result) = 0;
};

// AddTask is thread-safe; main callbacks run on the dispatcher thread.
class BgTaskManager : public FrequentTask {
 public:
  static constexpr uint32_t kMaxInFlightTasks = 4096;

  explicit BgTaskManager(EventDispatcher& dispatcher, uint32_t num_threads = 4);
  ~BgTaskManager() override;

  BgTaskManager(const BgTaskManager&) = delete;
  BgTaskManager& operator=(const BgTaskManager&) = delete;

  [[nodiscard]] auto AddTask(std::unique_ptr<BackgroundTask> task) -> Result<void>;

  [[nodiscard]] auto AddTask(std::function<Result<void>()> bg_work,
                             std::function<void(Result<void>)> main_callback) -> Result<void>;
  void StopAccepting();
  void CancelPending();

  [[nodiscard]] auto PendingCount() const -> uint32_t;
  [[nodiscard]] auto InFlightCount() const -> uint32_t;

 private:
  void DoTask() override;

  struct Impl;
  std::unique_ptr<Impl> impl_;
  FrequentTaskRegistration registration_;
};

}  // namespace atlas

#endif  // ATLAS_LIB_NETWORK_BG_TASK_MANAGER_H_
