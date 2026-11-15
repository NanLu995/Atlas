#include <atomic>
#include <chrono>
#include <future>
#include <thread>

#include <gtest/gtest.h>

#include "network/bg_task_manager.h"
#include "network/event_dispatcher.h"

using namespace atlas;

class BgTaskManagerTest : public ::testing::Test {
 protected:
  EventDispatcher dispatcher_{"test"};

  void SetUp() override { dispatcher_.SetMaxPollWait(Milliseconds(1)); }
};

TEST_F(BgTaskManagerTest, TaskExecutesAndCallsBack) {
  BgTaskManager mgr(dispatcher_, 2);

  std::atomic<bool> bg_done{false};
  std::atomic<bool> main_done{false};

  ASSERT_TRUE(mgr.AddTask(
      [&]() -> Result<void> {
        bg_done = true;
        return {};
      },
      [&](Result<void> result) {
        EXPECT_TRUE(result);
        main_done = true;
      }));

  // Wait for background task to complete
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Process dispatcher to run main-thread callback
  dispatcher_.ProcessOnce();

  EXPECT_TRUE(bg_done.load());
  EXPECT_TRUE(main_done.load());
}

TEST_F(BgTaskManagerTest, MultipleTasksAllComplete) {
  BgTaskManager mgr(dispatcher_, 2);

  constexpr int N = 10;
  std::atomic<int> bg_count{0};
  std::atomic<int> main_count{0};

  for (int i = 0; i < N; ++i) {
    ASSERT_TRUE(mgr.AddTask(
        [&]() -> Result<void> {
          ++bg_count;
          return {};
        },
        [&](Result<void> result) {
          EXPECT_TRUE(result);
          ++main_count;
        }));
  }

  // Wait for all background work
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  // Process multiple times to collect all callbacks
  for (int i = 0; i < 5; ++i) {
    dispatcher_.ProcessOnce();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  EXPECT_EQ(bg_count.load(), N);
  EXPECT_EQ(main_count.load(), N);
}

TEST_F(BgTaskManagerTest, AdmissionBoundsWorkUntilMainCompletion) {
  BgTaskManager manager(dispatcher_, 1);
  for (uint32_t i = 0; i < BgTaskManager::kMaxInFlightTasks; ++i)
    ASSERT_TRUE(manager.AddTask([]() -> Result<void> { return {}; },
                                [](Result<void> result) { EXPECT_TRUE(result); }));
  bool rejected_callback = false;
  auto rejected = manager.AddTask([]() -> Result<void> { return {}; },
                                  [&](Result<void>) { rejected_callback = true; });
  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.Error().Code(), ErrorCode::kRateLimited);
  EXPECT_EQ(manager.InFlightCount(), BgTaskManager::kMaxInFlightTasks);
  const auto deadline = Clock::now() + std::chrono::seconds(3);
  while (manager.InFlightCount() != 0 && Clock::now() < deadline) dispatcher_.ProcessOnce();
  ASSERT_EQ(manager.InFlightCount(), 0u);
  EXPECT_FALSE(rejected_callback);
  EXPECT_TRUE(manager.AddTask([]() -> Result<void> { return {}; },
                              [](Result<void> result) { EXPECT_TRUE(result); }));
}

TEST_F(BgTaskManagerTest, BackgroundTaskExceptionDoesNotCrash) {
  BgTaskManager mgr(dispatcher_, 2);

  bool main_called = false;
  ASSERT_TRUE(mgr.AddTask([]() -> Result<void> { throw std::runtime_error("boom"); },
                          [&](Result<void> result) {
                            ASSERT_FALSE(result);
                            EXPECT_EQ(result.Error().Code(), ErrorCode::kInternalError);
                            main_called = true;
                          }));

  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  dispatcher_.ProcessOnce();

  // Main callback should still be called even if bg task threw
  EXPECT_TRUE(main_called);
}

TEST_F(BgTaskManagerTest, CustomBackgroundTask) {
  BgTaskManager mgr(dispatcher_, 2);

  // Use shared state since the task object is destroyed after do_main_thread_task
  auto bg_result = std::make_shared<std::atomic<int>>(0);
  auto main_called = std::make_shared<std::atomic<bool>>(false);

  struct ComputeTask : BackgroundTask {
    int input;
    std::shared_ptr<std::atomic<int>> result_out;
    std::shared_ptr<std::atomic<bool>> main_flag;

    ComputeTask(int in, std::shared_ptr<std::atomic<int>> r, std::shared_ptr<std::atomic<bool>> m)
        : input(in), result_out(std::move(r)), main_flag(std::move(m)) {}

    auto DoBackgroundTask() -> Result<void> override {
      result_out->store(input * input);
      return {};
    }

    void DoMainThreadTask(Result<void> result) override {
      EXPECT_TRUE(result);
      main_flag->store(true);
    }
  };

  ASSERT_TRUE(mgr.AddTask(std::make_unique<ComputeTask>(7, bg_result, main_called)));

  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  dispatcher_.ProcessOnce();

  EXPECT_EQ(bg_result->load(), 49);
  EXPECT_TRUE(main_called->load());
}

TEST_F(BgTaskManagerTest, InFlightCountTracking) {
  BgTaskManager mgr(dispatcher_, 1);

  EXPECT_EQ(mgr.InFlightCount(), 0u);

  ASSERT_TRUE(mgr.AddTask(
      []() -> Result<void> {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        return {};
      },
      [](Result<void> result) { EXPECT_TRUE(result); }));

  // Should have 1 in flight
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  EXPECT_GE(mgr.InFlightCount(), 1u);

  // Wait for completion
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  dispatcher_.ProcessOnce();

  EXPECT_EQ(mgr.InFlightCount(), 0u);
}

TEST_F(BgTaskManagerTest, ShutdownWaitsForPendingTasks) {
  auto bg_done = std::make_shared<std::atomic<bool>>(false);
  auto main_done = std::make_shared<std::atomic<bool>>(false);

  {
    BgTaskManager mgr(dispatcher_, 2);

    ASSERT_TRUE(mgr.AddTask(
        [bg_done]() -> Result<void> {
          std::this_thread::sleep_for(std::chrono::milliseconds(200));
          bg_done->store(true);
          return {};
        },
        [main_done](Result<void> result) {
          EXPECT_TRUE(result);
          main_done->store(true);
        }));

    // Immediately destroy mgr — destructor should join threads and
    // process the completed task's main-thread callback.
  }

  // The background work must have completed (destructor joined threads)
  EXPECT_TRUE(bg_done->load());

  EXPECT_TRUE(main_done->load());
}

TEST_F(BgTaskManagerTest, InvalidInputAndStoppingReturnAdmissionErrors) {
  BgTaskManager manager(dispatcher_, 1);
  auto invalid = manager.AddTask(std::unique_ptr<BackgroundTask>{});
  ASSERT_FALSE(invalid);
  EXPECT_EQ(invalid.Error().Code(), ErrorCode::kInvalidArgument);
  invalid = manager.AddTask({}, {});
  ASSERT_FALSE(invalid);
  EXPECT_EQ(invalid.Error().Code(), ErrorCode::kInvalidArgument);
  manager.StopAccepting();
  bool called = false;
  auto stopped =
      manager.AddTask([]() -> Result<void> { return {}; }, [&](Result<void>) { called = true; });
  ASSERT_FALSE(stopped);
  EXPECT_EQ(stopped.Error().Code(), ErrorCode::kReceiverGone);
  dispatcher_.ProcessOnce();
  EXPECT_FALSE(called);
  EXPECT_EQ(manager.InFlightCount(), 0u);
}

TEST_F(BgTaskManagerTest, CancellationSkipsQueuedWorkAndReportsOnceOnMainThread) {
  BgTaskManager manager(dispatcher_, 1);
  std::promise<void> started, release;
  auto started_future = started.get_future();
  auto released = release.get_future();
  int callbacks = 0;
  std::atomic<bool> queued_ran{false};
  ASSERT_TRUE(manager.AddTask(
      [&]() -> Result<void> {
        started.set_value();
        released.wait_for(std::chrono::seconds(3));
        return {};
      },
      [&](Result<void> result) {
        EXPECT_TRUE(result);
        ++callbacks;
      }));
  ASSERT_EQ(started_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
  const auto main_thread = std::this_thread::get_id();
  ASSERT_TRUE(manager.AddTask(
      [&]() -> Result<void> {
        queued_ran = true;
        return {};
      },
      [&](Result<void> result) {
        ASSERT_FALSE(result);
        EXPECT_EQ(result.Error().Code(), ErrorCode::kCancelled);
        EXPECT_EQ(std::this_thread::get_id(), main_thread);
        ++callbacks;
      }));
  manager.CancelPending();
  release.set_value();
  const auto deadline = Clock::now() + std::chrono::seconds(3);
  while (manager.InFlightCount() != 0 && Clock::now() < deadline) dispatcher_.ProcessOnce();
  EXPECT_EQ(manager.InFlightCount(), 0u);
  EXPECT_FALSE(queued_ran);
  EXPECT_EQ(callbacks, 2);
  dispatcher_.ProcessOnce();
  EXPECT_EQ(callbacks, 2);
}

TEST_F(BgTaskManagerTest, WorkFailureIsDeliveredWithoutBecomingSuccess) {
  BgTaskManager manager(dispatcher_, 1);
  bool called = false;
  ASSERT_TRUE(manager.AddTask([]() -> Result<void> { return Error{ErrorCode::kIoError}; },
                              [&](Result<void> result) {
                                ASSERT_FALSE(result);
                                EXPECT_EQ(result.Error().Code(), ErrorCode::kIoError);
                                called = true;
                              }));
  const auto deadline = Clock::now() + std::chrono::seconds(3);
  while (!called && Clock::now() < deadline) dispatcher_.ProcessOnce();
  EXPECT_TRUE(called);
}
