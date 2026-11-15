#include "network/bg_task_manager.h"

#include "foundation/callback_utils.h"
#include "foundation/log.h"
#include "network/event_dispatcher.h"
#include "platform/threading.h"

namespace atlas {

class LambdaTask : public BackgroundTask {
 public:
  LambdaTask(std::function<Result<void>()> bg, std::function<void(Result<void>)> main)
      : bg_work_(std::move(bg)), main_callback_(std::move(main)) {}

  auto DoBackgroundTask() -> Result<void> override { return bg_work_(); }
  void DoMainThreadTask(Result<void> result) override { main_callback_(std::move(result)); }

 private:
  std::function<Result<void>()> bg_work_;
  std::function<void(Result<void>)> main_callback_;
};

struct BgTaskManager::Impl {
  EventDispatcher& dispatcher;
  ThreadPool pool;
  std::mutex admission_mutex;
  bool accepting{true};
  std::atomic<bool> cancel_pending{false};
  std::mutex completed_mutex;
  struct CompletedTask {
    std::unique_ptr<BackgroundTask> task;
    Result<void> result;
  };
  std::vector<CompletedTask> completed;
  std::atomic<uint32_t> in_flight{0};

  Impl(EventDispatcher& d, uint32_t threads) : dispatcher(d), pool(threads) {}
};

BgTaskManager::BgTaskManager(EventDispatcher& dispatcher, uint32_t num_threads)
    : impl_(std::make_unique<Impl>(dispatcher, num_threads)),
      registration_(impl_->dispatcher.AddFrequentTask(this)) {}

BgTaskManager::~BgTaskManager() {
  StopAccepting();
  registration_.Reset();
  impl_->pool.Shutdown();
  DoTask();
}

auto BgTaskManager::AddTask(std::unique_ptr<BackgroundTask> task) -> Result<void> {
  if (!task) return Error{ErrorCode::kInvalidArgument, "background task is null"};
  std::lock_guard admission(impl_->admission_mutex);
  if (!impl_->accepting)
    return Error{ErrorCode::kReceiverGone, "background task manager is stopping"};
  if (impl_->in_flight.load(std::memory_order_acquire) >= kMaxInFlightTasks)
    return Error{ErrorCode::kRateLimited, "background task capacity reached"};
  impl_->in_flight.fetch_add(1, std::memory_order_relaxed);

  auto* raw = task.release();
  impl_->pool.Submit([this, raw]() {
    std::unique_ptr<BackgroundTask> owned(raw);
    Result<void> result{Error{ErrorCode::kCancelled, "background task cancelled before execution"}};
    if (!impl_->cancel_pending.load(std::memory_order_acquire)) {
      result = Error{ErrorCode::kInternalError, "background task threw an exception"};
      SafeInvoke("BackgroundTask::do_background_task", [&] { result = owned->DoBackgroundTask(); });
    }
    {
      std::lock_guard lock(impl_->completed_mutex);
      impl_->completed.push_back({std::move(owned), std::move(result)});
    }
  });
  return {};
}

auto BgTaskManager::AddTask(std::function<Result<void>()> bg_work,
                            std::function<void(Result<void>)> main_callback) -> Result<void> {
  if (!bg_work || !main_callback)
    return Error{ErrorCode::kInvalidArgument, "background work and callback are required"};
  return AddTask(std::make_unique<LambdaTask>(std::move(bg_work), std::move(main_callback)));
}

void BgTaskManager::StopAccepting() {
  std::lock_guard admission(impl_->admission_mutex);
  impl_->accepting = false;
}

void BgTaskManager::CancelPending() {
  std::lock_guard admission(impl_->admission_mutex);
  impl_->accepting = false;
  impl_->cancel_pending.store(true, std::memory_order_release);
}

auto BgTaskManager::PendingCount() const -> uint32_t {
  return impl_->pool.PendingTasks();
}

auto BgTaskManager::InFlightCount() const -> uint32_t {
  return impl_->in_flight.load(std::memory_order_acquire);
}

void BgTaskManager::DoTask() {
  std::vector<Impl::CompletedTask> tasks;
  {
    std::lock_guard lock(impl_->completed_mutex);
    tasks.swap(impl_->completed);
  }

  for (auto& completed : tasks) {
    SafeInvoke("BackgroundTask::do_main_thread_task",
               [&] { completed.task->DoMainThreadTask(std::move(completed.result)); });
    impl_->in_flight.fetch_sub(1, std::memory_order_release);
  }
}

}  // namespace atlas
