#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <vector>

#include <gtest/gtest.h>

#include "coro/fire_and_forget.h"
#include "server/await_machined_query.h"
#include "server/common_messages.h"
#include "server/server_app.h"
#include "test_null_channel.h"
#include "test_shutdown_access.h"

using namespace atlas;

namespace atlas {
struct MachinedClientTestAccess {
  static void SetNextQueryId(MachinedClient& client, uint64_t id) {
    client.next_query_request_id_ = id;
  }
  static void ExpireQueries(MachinedClient& client) {
    for (auto& [_, query] : client.pending_queries_) {
      query.issued_at = Clock::now() - client.request_timeout_;
    }
  }
  static auto PendingQueries(const MachinedClient& client) -> size_t {
    return client.pending_queries_.size();
  }
};
}  // namespace atlas

// Minimal concrete ServerApp — no network binding needed for unit tests.
// Overrides Init() to skip port binding and shorten the tick interval.
class TestServerApp : public ServerApp {
 public:
  TestServerApp(EventDispatcher& d, NetworkInterface& n) : ServerApp(d, n) {}

  // Lifecycle event tracking
  bool init_called{false};
  bool fini_called{false};
  bool run_called{false};

  std::vector<std::string> hook_order;

  int on_end_count{0};
  int on_start_count{0};
  int on_tick_count{0};

  int max_ticks{0};  // shutdown after this many OnTickComplete() calls

 protected:
  auto Init(int argc, char* argv[]) -> bool override {
    if (!ServerApp::Init(argc, argv)) return false;
    init_called = true;
    hook_order.push_back("init");
    return true;
  }

  void Fini() override {
    fini_called = true;
    hook_order.push_back("fini");
    ServerApp::Fini();
  }

  auto RunLoop() -> bool override {
    run_called = true;
    hook_order.push_back("run");
    return ServerApp::RunLoop();
  }

  void OnEndOfTick() override {
    ++on_end_count;
    hook_order.push_back("end");
  }

  void OnStartOfTick() override {
    ++on_start_count;
    hook_order.push_back("start");
  }

  void OnTickComplete() override {
    ++on_tick_count;
    hook_order.push_back("tick");
    if (max_ticks > 0 && on_tick_count >= max_ticks) Shutdown();
  }
};

// Build a minimal argv that avoids port binding (no --machined resolution needed)
struct MinimalArgv {
  std::vector<std::string> storage{"exe", "--type", "machined", "--update-hertz", "100"};
  std::vector<char*> ptrs;

  MinimalArgv() {
    for (auto& s : storage) ptrs.push_back(s.data());
  }

  int argc() { return static_cast<int>(ptrs.size()); }
  char** argv() { return ptrs.data(); }
};

TEST(CommonMessages, HeartbeatRoundTrip) {
  msg::Heartbeat hb{42, 0.75f};

  BinaryWriter w;
  hb.Serialize(w);

  BinaryReader r(w.Data());
  auto result = msg::Heartbeat::Deserialize(r);
  ASSERT_TRUE(result.HasValue());
  EXPECT_EQ(result->game_time, 42u);
  EXPECT_FLOAT_EQ(result->load, 0.75f);
}

TEST(CommonMessages, ShutdownRequestRoundTrip) {
  msg::ShutdownRequest req{2};

  BinaryWriter w;
  req.Serialize(w);

  BinaryReader r(w.Data());
  auto result = msg::ShutdownRequest::Deserialize(r);
  ASSERT_TRUE(result.HasValue());
  EXPECT_EQ(result->reason, 2u);
}

TEST(CommonMessages, HeartbeatDescriptor) {
  const auto& desc = msg::Heartbeat::Descriptor();
  EXPECT_EQ(desc.id, 100);
  EXPECT_EQ(desc.length_style, MessageLengthStyle::kFixed);
}

TEST(CommonMessages, ShutdownRequestDescriptor) {
  const auto& desc = msg::ShutdownRequest::Descriptor();
  EXPECT_EQ(desc.id, 101);
  EXPECT_EQ(desc.length_style, MessageLengthStyle::kFixed);
}

TEST(ServerAppDeathTest, RemovedDatabaseBackendReportsErrorBeforeRuntimeStarts) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  EventDispatcher dispatcher("config_error");
  NetworkInterface network(dispatcher);
  TestServerApp app(dispatcher, network);
  char executable[] = "server";
  char option[] = "--db-type";
  char backend[] = "xml";
  char* args[] = {executable, option, backend};
  EXPECT_EXIT(std::exit(app.RunApp(3, args)), ::testing::ExitedWithCode(1),
              "unsupported database backend 'xml'; use mysql");
}

TEST(ServerApp, InitRunFiniOrder) {
  EventDispatcher dispatcher("test");
  NetworkInterface network(dispatcher);
  TestServerApp app(dispatcher, network);
  app.max_ticks = 1;

  MinimalArgv args;
  int exit_code = app.RunApp(args.argc(), args.argv());
  EXPECT_EQ(exit_code, 0);
  EXPECT_GT(app.Config().process_instance, 0u);
  EXPECT_LE(app.Config().process_instance, UINT64_MAX >> 1);

  // init and fini must both have been called
  EXPECT_TRUE(app.init_called);
  EXPECT_TRUE(app.fini_called);
  EXPECT_TRUE(app.run_called);

  // Sequence: init → run → (ticks) → fini
  ASSERT_GE(app.hook_order.size(), 3u);
  EXPECT_EQ(app.hook_order.front(), "init");
  EXPECT_EQ(app.hook_order.back(), "fini");
}

TEST(ServerApp, ShutdownPumpsCompletionsBeforeStoppingWithNoPendingOperations) {
  EventDispatcher dispatcher("shutdown_completion");
  NetworkInterface network(dispatcher);
  class CompletionApp : public TestServerApp {
   public:
    using TestServerApp::TestServerApp;
    int drain_count{0};

   protected:
    void OnShutdownTick() override { ++drain_count; }
  } app(dispatcher, network);
  app.max_ticks = 1;
  char executable[] = "server";
  char type_option[] = "--type";
  char type[] = "machined";
  char hertz_option[] = "--update-hertz";
  char hertz[] = "10";
  char* args[] = {executable, type_option, type, hertz_option, hertz};
  EXPECT_EQ(app.RunApp(5, args), 0);
  EXPECT_GT(app.drain_count, 0);
  EXPECT_EQ(app.on_tick_count, 1);
}

TEST(ServerApp, TickHookOrder) {
  EventDispatcher dispatcher("test");
  NetworkInterface network(dispatcher);
  TestServerApp app(dispatcher, network);
  app.max_ticks = 1;

  MinimalArgv args;
  app.RunApp(args.argc(), args.argv());

  // Within a tick, order must be: end → start → tick
  // Find the first occurrence of each hook after "run"
  auto it = std::find(app.hook_order.begin(), app.hook_order.end(), "run");
  ASSERT_NE(it, app.hook_order.end());

  std::vector<std::string> tick_hooks(it + 1, app.hook_order.end());
  // Remove "fini" from the end
  tick_hooks.erase(std::remove(tick_hooks.begin(), tick_hooks.end(), "fini"), tick_hooks.end());

  // Expect at least one complete cycle: end, start, tick
  ASSERT_GE(tick_hooks.size(), 3u);
  EXPECT_EQ(tick_hooks[0], "end");
  EXPECT_EQ(tick_hooks[1], "start");
  EXPECT_EQ(tick_hooks[2], "tick");
}

TEST(ServerApp, ShutdownStopsRun) {
  EventDispatcher dispatcher("test");
  NetworkInterface network(dispatcher);
  TestServerApp app(dispatcher, network);
  app.max_ticks = 3;

  MinimalArgv args;
  int exit_code = app.RunApp(args.argc(), args.argv());
  EXPECT_EQ(exit_code, 0);
  EXPECT_EQ(app.on_tick_count, 3);
}

TEST(ServerApp, ShutdownKeepsLoopAliveUntilAcceptedWorkCompletes) {
  EventDispatcher dispatcher("shutdown_drain");
  NetworkInterface network(dispatcher);
  class DrainApp : public TestServerApp {
   public:
    using TestServerApp::TestServerApp;
    bool completed{false};
    bool completed_before_fini{false};

   protected:
    auto PendingShutdownOperations() const -> std::size_t override {
      return TestServerApp::PendingShutdownOperations() + (completed ? 0 : 1);
    }
    void OnTickComplete() override {
      (void)Dispatcher().AddTimer(std::chrono::milliseconds(20),
                                  [this](TimerHandle) { completed = true; });
      Shutdown();
    }
    void Fini() override {
      completed_before_fini = completed;
      TestServerApp::Fini();
    }
  } app(dispatcher, network);
  MinimalArgv args;
  EXPECT_EQ(app.RunApp(args.argc(), args.argv()), 0);
  EXPECT_TRUE(app.completed);
  EXPECT_TRUE(app.completed_before_fini);
}

TEST(ServerApp, FailedDrainReturnsFailureAfterCleanup) {
  EventDispatcher dispatcher("shutdown_failure");
  NetworkInterface network(dispatcher);
  class FailedApp : public TestServerApp {
   public:
    using TestServerApp::TestServerApp;

   protected:
    void BeginShutdown() override {
      TestServerApp::BeginShutdown();
      MarkShutdownFailed(Error{ErrorCode::kIoError, "test save failed"});
    }
  } app(dispatcher, network);
  app.max_ticks = 1;
  MinimalArgv args;
  EXPECT_EQ(app.RunApp(args.argc(), args.argv()), 1);
  EXPECT_TRUE(app.fini_called);
}

TEST(ServerAppDeathTest, ExpiredDrainExitsFailureBeforeBlockingCleanup) {
  ASSERT_EXIT(
      {
        EventDispatcher dispatcher("shutdown_expired");
        NetworkInterface network(dispatcher);
        class ExpiredApp : public TestServerApp {
         public:
          using TestServerApp::TestServerApp;

         protected:
          auto PendingShutdownOperations() const -> std::size_t override { return 1; }
          void OnTickComplete() override {
            Shutdown();
            ServerAppShutdownTestAccess::Expire(*this);
          }
          void Fini() override { std::abort(); }
        } app(dispatcher, network);
        MinimalArgv args;
        app.RunApp(args.argc(), args.argv());
        std::_Exit(0);
      },
      ::testing::ExitedWithCode(EXIT_FAILURE), ".*");
}

TEST(ServerApp, UpdatableCalledEachTick) {
  EventDispatcher dispatcher("test");
  NetworkInterface network(dispatcher);
  TestServerApp app(dispatcher, network);
  app.max_ticks = 3;

  struct Counter : Updatable {
    int count{0};
    void Update() override { ++count; }
  } counter;

  // Register updatable before RunApp — Init() will add the tick timer,
  // but we can register the updatable before or after Init.
  // We'll do it by subclassing the Init hook.
  // For simplicity, register directly before RunApp:
  // The updatable needs to be registered on the app. We expose it via a
  // wrapper that calls RegisterForUpdate from within Init.
  struct CounterApp : TestServerApp {
    Counter* counter_ptr{nullptr};

    CounterApp(EventDispatcher& d, NetworkInterface& n) : TestServerApp(d, n) {}

    auto Init(int argc, char* argv[]) -> bool override {
      if (!TestServerApp::Init(argc, argv)) return false;
      RegisterForUpdate(counter_ptr);
      return true;
    }
  };

  CounterApp capp(dispatcher, network);
  capp.counter_ptr = &counter;
  capp.max_ticks = 3;

  MinimalArgv args;
  capp.RunApp(args.argc(), args.argv());

  EXPECT_EQ(counter.count, 3);
}

TEST(ServerApp, GameTimeProvidesMonotonicallyIncreasingFrameCount) {
  EventDispatcher dispatcher("test");
  NetworkInterface network(dispatcher);
  TestServerApp app(dispatcher, network);
  app.max_ticks = 5;

  MinimalArgv args;
  app.RunApp(args.argc(), args.argv());

  EXPECT_GE(app.GameTime(), 5u);
}

TEST(ServerApp, WatcherRegisteredAfterInit) {
  EventDispatcher dispatcher("test");
  NetworkInterface network(dispatcher);
  TestServerApp app(dispatcher, network);
  app.max_ticks = 1;

  MinimalArgv args;
  app.RunApp(args.argc(), args.argv());

  // Standard watchers must be present
  EXPECT_TRUE(app.GetWatcherRegistry().Get("app/type").has_value());
  EXPECT_TRUE(app.GetWatcherRegistry().Get("app/name").has_value());
  EXPECT_TRUE(app.GetWatcherRegistry().Get("tick/slow_count").has_value());
  EXPECT_TRUE(app.GetWatcherRegistry().Get("tick/total_count").has_value());
  EXPECT_EQ(app.GetWatcherRegistry().Get("app/shutdown/state"), "complete");
  EXPECT_EQ(app.GetWatcherRegistry().Get("app/shutdown/pending_operations"), "0");
  EXPECT_EQ(app.GetWatcherRegistry().Get("app/shutdown/remaining_ms"), "0");
}

TEST(ServerApp, ShutdownDiagnosticsReflectPendingFailureAndDeadline) {
  class DiagnosticApp : public ServerApp {
   public:
    using ServerApp::MarkShutdownFailed;
    using ServerApp::RegisterWatchers;
    using ServerApp::ServerApp;
    std::size_t pending{0};
    auto PendingShutdownOperations() const -> std::size_t override { return pending; }
  };
  EventDispatcher dispatcher{"shutdown_diagnostics"};
  dispatcher.SetMaxPollWait(Milliseconds(1));
  NetworkInterface network{dispatcher};
  DiagnosticApp app{dispatcher, network};
  app.RegisterWatchers();
  auto& watchers = app.GetWatcherRegistry();
  EXPECT_EQ(watchers.Get("app/shutdown/state"), "running");
  EXPECT_EQ(watchers.Get("app/ready"), "true");
  EXPECT_EQ(watchers.Get("app/not_ready_reason"), "");
  EXPECT_EQ(watchers.Get("app/shutdown/remaining_ms"), "0");
  app.pending = 2;
  app.Shutdown();
  EXPECT_EQ(watchers.Get("app/shutdown/state"), "draining");
  EXPECT_EQ(watchers.Get("app/ready"), "false");
  EXPECT_EQ(watchers.Get("app/not_ready_reason"), "shutting_down");
  EXPECT_EQ(watchers.Get("app/shutdown/pending_operations"), "2");
  const auto remaining = watchers.Get("app/shutdown/remaining_ms");
  ASSERT_TRUE(remaining);
  EXPECT_GT(std::stoll(*remaining), 0);
  EXPECT_LE(std::stoll(*remaining), 30000);
  app.MarkShutdownFailed(Error{ErrorCode::kIoError, "save failed"});
  EXPECT_EQ(watchers.Get("app/shutdown/state"), "failed");
  ServerAppShutdownTestAccess::Expire(app);
  const auto deadline = Clock::now() + std::chrono::seconds(1);
  while (Clock::now() < deadline && watchers.Get("app/shutdown/state") != "timed_out")
    dispatcher.ProcessOnce();
  EXPECT_EQ(watchers.Get("app/shutdown/state"), "timed_out");
  EXPECT_EQ(watchers.Get("app/shutdown/remaining_ms"), "0");
  EXPECT_EQ(watchers.Get("app/shutdown/pending_operations"), "2");
}

TEST(ServerApp, ConfigLoadedFromArgs) {
  EventDispatcher dispatcher("test");
  NetworkInterface network(dispatcher);
  TestServerApp app(dispatcher, network);
  app.max_ticks = 1;

  std::vector<std::string> storage{"exe",       "--type",         "machined", "--name",
                                   "test_proc", "--update-hertz", "50"};
  std::vector<char*> ptrs;
  for (auto& s : storage) ptrs.push_back(s.data());

  app.RunApp(static_cast<int>(ptrs.size()), ptrs.data());

  EXPECT_EQ(app.Config().process_type, ProcessType::kMachined);
  EXPECT_EQ(app.Config().process_name, "test_proc");
  EXPECT_EQ(app.Config().update_hertz, 50);
}

// Phase-alignment math for cellapp tick sync — `now < epoch` aligns
// forward to `epoch`, `now == epoch` keeps it, `now` past N full periods
// rounds up to N+1.
TEST(ServerApp, NextTickBoundary_PhaseAlignment) {
  using namespace std::chrono_literals;
  const auto period = 50ms;
  const TimePoint epoch = TimePoint{} + 1000ms;

  // now == epoch: already on the boundary, no shift.
  EXPECT_EQ(ServerApp::NextTickBoundary(epoch, epoch, period), epoch);

  // now mid-period: round up to the next aligned boundary.
  EXPECT_EQ(ServerApp::NextTickBoundary(epoch + 30ms, epoch, period), epoch + 50ms);

  // now exactly on a boundary: stays put.
  EXPECT_EQ(ServerApp::NextTickBoundary(epoch + 100ms, epoch, period), epoch + 100ms);

  // now before epoch: returns epoch (negative since_ns div period = 0).
  EXPECT_EQ(ServerApp::NextTickBoundary(epoch - 20ms, epoch, period), epoch);

  // Zero / negative period: caller has no aligned tick to converge to.
  EXPECT_EQ(ServerApp::NextTickBoundary(epoch, epoch, Duration::zero()), epoch);
}

class MachinedSourceTest : public ::testing::Test {
 protected:
  EventDispatcher dispatcher_{"machined_source"};
  NetworkInterface controller_{dispatcher_};
  NetworkInterface network_{dispatcher_};
  MachinedClient client_{dispatcher_, network_};
  Channel* channel_{nullptr};

  void SetUp() override {
    dispatcher_.SetMaxPollWait(Milliseconds(1));
    ASSERT_TRUE(controller_.StartTcpServer(Address("127.0.0.1", 0)).HasValue());
    ASSERT_TRUE(client_.Connect(controller_.TcpAddress()));
    const auto deadline = Clock::now() + std::chrono::seconds(2);
    while (!client_.IsConnected() && Clock::now() < deadline) dispatcher_.ProcessOnce();
    ASSERT_TRUE(client_.IsConnected());
    channel_ = network_.FindChannel(controller_.TcpAddress());
    ASSERT_NE(channel_, nullptr);
  }

  template <NetworkMessage Msg>
  void Dispatch(Channel* channel, const Msg& msg) {
    BinaryWriter writer;
    msg.Serialize(writer);
    BinaryReader reader(writer.Data());
    ASSERT_TRUE(network_.InterfaceTable()
                    .Dispatch(controller_.TcpAddress(), channel, Msg::Descriptor().id, reader)
                    .HasValue());
  }
};

TEST_F(MachinedSourceTest, AwaitedRegistryQueryHandlesInlineCancellationAndLateCompletion) {
  CancellationSource cancel;
  uint32_t calls = 0;
  auto run = [&]() -> FireAndForget {
    auto result = co_await AwaitMachinedQuery(client_, ProcessType::kBaseApp, cancel.Token());
    EXPECT_FALSE(result);
    if (!result) {
      EXPECT_EQ(result.Error().Code(), ErrorCode::kCancelled);
    }
    ++calls;
  };
  run();
  EXPECT_EQ(calls, 0u);
  EXPECT_EQ(MachinedClientTestAccess::PendingQueries(client_), 1u);
  cancel.RequestCancellation();
  EXPECT_EQ(calls, 1u);
  MachinedClientTestAccess::ExpireQueries(client_);
  client_.Tick();
  EXPECT_EQ(calls, 1u);
  EXPECT_EQ(MachinedClientTestAccess::PendingQueries(client_), 0u);
  run();
  EXPECT_EQ(calls, 2u);
  EXPECT_EQ(MachinedClientTestAccess::PendingQueries(client_), 0u);
}

TEST(ServerApp, AwaitedRegistryQueryCompletesInlineWhenDisconnected) {
  EventDispatcher dispatcher;
  NetworkInterface network(dispatcher);
  MachinedClient client(dispatcher, network);
  bool completed = false;
  auto run = [&]() -> FireAndForget {
    auto result = co_await AwaitMachinedQuery(client, ProcessType::kBaseApp);
    EXPECT_FALSE(result);
    completed = true;
  };
  run();
  EXPECT_TRUE(completed);
}

TEST_F(MachinedSourceTest, QueryResponseRequiresCurrentMachinedChannel) {
  uint32_t calls = 0;
  const auto id = client_.QueryAsync(ProcessType::kBaseApp, [&](auto) { ++calls; });
  test_support::TestNullChannel impostor(dispatcher_, network_.InterfaceTable(),
                                         controller_.TcpAddress());
  machined::QueryResponse response;
  response.request_id = id;
  Dispatch(nullptr, response);
  Dispatch(&impostor, response);
  EXPECT_EQ(calls, 0u);
  Dispatch(channel_, response);
  EXPECT_EQ(calls, 1u);
  Dispatch(channel_, response);
  EXPECT_EQ(calls, 1u);
}

TEST_F(MachinedSourceTest, ConcurrentRegistryQueriesDoNotCompleteEachOther) {
  std::array<uint32_t, 3> calls{};
  std::array<uint64_t, 3> ids{};
  const std::array roles{ProcessType::kCellAppMgr, ProcessType::kBaseAppMgr,
                         ProcessType::kDbAppMgr};
  for (size_t i = 0; i < roles.size(); ++i) {
    ids[i] = client_.QueryAsync(roles[i], [&, i](MachinedClient::QueryResult result) {
      ASSERT_TRUE(result);
      ASSERT_EQ(result->size(), 1u);
      EXPECT_EQ(result->front().process_type, roles[i]);
      ++calls[i];
    });
    ASSERT_NE(ids[i], 0u);
    if (i != 0) {
      EXPECT_GT(ids[i], ids[i - 1]);
    }
    EXPECT_EQ(calls[i], 0u);
  }
  for (size_t i = roles.size(); i-- > 0;) {
    machined::QueryResponse response;
    response.request_id = ids[i];
    response.processes.emplace_back().process_type = roles[i];
    Dispatch(channel_, response);
    Dispatch(channel_, response);
    EXPECT_EQ(calls[i], 1u);
  }
  EXPECT_EQ(calls, (std::array<uint32_t, 3>{1, 1, 1}));
  EXPECT_EQ(MachinedClientTestAccess::PendingQueries(client_), 0u);
}

TEST_F(MachinedSourceTest, RegistryTimeoutAndLateReplyCannotCompleteNextQuery) {
  uint32_t expired_calls = 0;
  const auto expired_id =
      client_.QueryAsync(ProcessType::kBaseApp, [&](MachinedClient::QueryResult result) {
        ASSERT_FALSE(result);
        EXPECT_EQ(result.Error().Code(), ErrorCode::kTimeout);
        ++expired_calls;
      });
  MachinedClientTestAccess::ExpireQueries(client_);
  client_.Tick();
  EXPECT_EQ(expired_calls, 1u);
  uint32_t calls = 0;
  const auto id =
      client_.QueryAsync(ProcessType::kBaseApp, [&](MachinedClient::QueryResult result) {
        ASSERT_TRUE(result);
        EXPECT_TRUE(result->empty());
        ++calls;
      });
  EXPECT_GT(id, expired_id);
  machined::QueryResponse response;
  response.request_id = expired_id;
  Dispatch(channel_, response);
  EXPECT_EQ(calls, 0u);
  response.request_id = 0;
  Dispatch(channel_, response);
  EXPECT_EQ(calls, 0u);
  response.request_id = id;
  Dispatch(channel_, response);
  client_.Tick();
  EXPECT_EQ(expired_calls, 1u);
  EXPECT_EQ(calls, 1u);
}

TEST_F(MachinedSourceTest, RegistryQueryLimitRejectsAndExpiredQueriesReleaseCapacity) {
  uint32_t expired = 0;
  for (size_t i = 0; i < MachinedClient::kMaxPendingQueries; ++i) {
    ASSERT_NE(client_.QueryAsync(ProcessType::kBaseApp,
                                 [&](MachinedClient::QueryResult result) {
                                   ASSERT_FALSE(result);
                                   EXPECT_EQ(result.Error().Code(), ErrorCode::kTimeout);
                                   ++expired;
                                 }),
              0u);
  }
  uint32_t rejected = 0;
  EXPECT_EQ(client_.QueryAsync(ProcessType::kBaseApp,
                               [&](MachinedClient::QueryResult result) {
                                 ASSERT_FALSE(result);
                                 EXPECT_EQ(result.Error().Code(), ErrorCode::kRateLimited);
                                 ++rejected;
                               }),
            0u);
  EXPECT_EQ(rejected, 1u);
  MachinedClientTestAccess::ExpireQueries(client_);
  client_.Tick();
  EXPECT_EQ(expired, MachinedClient::kMaxPendingQueries);
  EXPECT_EQ(MachinedClientTestAccess::PendingQueries(client_), 0u);
  EXPECT_NE(client_.QueryAsync(ProcessType::kBaseApp, [](auto) {}), 0u);
}

TEST_F(MachinedSourceTest, RegistryIdsExhaustWithoutReusingOutstandingIds) {
  MachinedClientTestAccess::SetNextQueryId(client_, UINT64_MAX);
  uint32_t calls = 0;
  const auto id =
      client_.QueryAsync(ProcessType::kBaseApp, [&](MachinedClient::QueryResult result) {
        EXPECT_TRUE(result);
        ++calls;
      });
  ASSERT_EQ(id, UINT64_MAX);
  uint32_t rejected = 0;
  EXPECT_EQ(client_.QueryAsync(ProcessType::kBaseApp,
                               [&](MachinedClient::QueryResult result) {
                                 ASSERT_FALSE(result);
                                 EXPECT_EQ(result.Error().Code(), ErrorCode::kOutOfRange);
                                 ++rejected;
                               }),
            0u);
  EXPECT_EQ(rejected, 1u);
  EXPECT_EQ(calls, 0u);
  machined::QueryResponse response;
  response.request_id = id;
  Dispatch(channel_, response);
  EXPECT_EQ(calls, 1u);
}

TEST_F(MachinedSourceTest, SynchronousRegistryQueryHasIndependentCompletion) {
  ASSERT_TRUE(controller_.InterfaceTable().RegisterTypedHandler<machined::QueryMessage>(
      [&](const Address&, Channel* channel, const machined::QueryMessage& message) {
        machined::QueryResponse response;
        response.request_id = message.request_id;
        response.processes.emplace_back().process_type = message.process_type;
        ASSERT_TRUE(channel->SendMessage(response));
      }));
  uint32_t calls = 0;
  client_.QueryAsync(ProcessType::kBaseApp, [&](MachinedClient::QueryResult result) {
    ASSERT_TRUE(result);
    ASSERT_EQ(result->size(), 1u);
    EXPECT_EQ(result->front().process_type, ProcessType::kBaseApp);
    ++calls;
  });
  auto result = client_.QuerySync(ProcessType::kCellApp, Milliseconds(500));
  ASSERT_TRUE(result);
  ASSERT_EQ(result->size(), 1u);
  EXPECT_EQ(result->front().process_type, ProcessType::kCellApp);
  EXPECT_EQ(calls, 1u);
  EXPECT_EQ(MachinedClientTestAccess::PendingQueries(client_), 0u);
}

TEST_F(MachinedSourceTest, SynchronousRegistryTimeoutRemovesStackCallback) {
  const auto result = client_.QuerySync(ProcessType::kBaseApp, Duration::zero());
  ASSERT_FALSE(result);
  EXPECT_EQ(result.Error().Code(), ErrorCode::kTimeout);
  EXPECT_EQ(MachinedClientTestAccess::PendingQueries(client_), 0u);
  machined::QueryResponse response;
  response.request_id = 1;
  Dispatch(channel_, response);
}

TEST_F(MachinedSourceTest, WatcherResponseRequiresCurrentMachinedChannel) {
  uint32_t calls = 0;
  const auto id = client_.QueryWatcher(ProcessType::kBaseApp, "", "app/name",
                                       [&](bool, const auto&, const auto&) { ++calls; });
  ASSERT_NE(id, 0u);
  test_support::TestNullChannel impostor(dispatcher_, network_.InterfaceTable(),
                                         controller_.TcpAddress());
  machined::WatcherResponse response;
  response.request_id = id;
  response.found = true;
  Dispatch(nullptr, response);
  Dispatch(&impostor, response);
  EXPECT_EQ(calls, 0u);
  Dispatch(channel_, response);
  EXPECT_EQ(calls, 1u);
  Dispatch(channel_, response);
  EXPECT_EQ(calls, 1u);
}

TEST_F(MachinedSourceTest, DisconnectedMachinedCannotCompletePendingQuery) {
  uint32_t calls = 0;
  const auto id =
      client_.QueryAsync(ProcessType::kBaseApp, [&](MachinedClient::QueryResult result) {
        ASSERT_FALSE(result);
        EXPECT_EQ(result.Error().Code(), ErrorCode::kConnectionReset);
        ++calls;
      });
  IntrusivePtr<Channel> old_channel{channel_};
  network_.DisconnectChannel(controller_.TcpAddress());
  machined::QueryResponse response;
  response.request_id = id;
  Dispatch(old_channel.get(), response);
  EXPECT_EQ(calls, 0u);
  client_.Tick();
  EXPECT_EQ(calls, 1u);
}

TEST(ServerApp, RejectsUnregisteredControlCommands) {
  class App final : public TestServerApp {
   public:
    using TestServerApp::TestServerApp;
    int value = 0;

   protected:
    void OnTickComplete() override {
      GetWatcherRegistry().AddRw("security/value", value);
      test_support::TestNullChannel impostor(Dispatcher(), Network().InterfaceTable(),
                                             Address("127.0.0.1", 1234));
      machined::WatcherForward command;
      command.op = machined::WatcherOp::kSet;
      command.watcher_path = "security/value";
      command.set_value = "7";
      BinaryWriter writer;
      command.Serialize(writer);
      BinaryReader reader(writer.Data());
      EXPECT_TRUE(
          Network()
              .InterfaceTable()
              .Dispatch(impostor.RemoteAddress(), &impostor, command.Descriptor().id, reader)
              .HasValue());
      EXPECT_EQ(value, 0);
      BinaryWriter shutdown;
      msg::ShutdownRequest request{0};
      request.Serialize(shutdown);
      BinaryReader shutdown_reader(shutdown.Data());
      EXPECT_TRUE(Network()
                      .InterfaceTable()
                      .Dispatch(impostor.RemoteAddress(), &impostor, request.Descriptor().id,
                                shutdown_reader)
                      .HasValue());
      EXPECT_FALSE(IsShuttingDown());
      Shutdown();
    }
  };
  EventDispatcher dispatcher("unregistered_control");
  NetworkInterface network(dispatcher);
  App app(dispatcher, network);
  MinimalArgv args;
  EXPECT_EQ(app.RunApp(args.argc(), args.argv()), 0);
}
