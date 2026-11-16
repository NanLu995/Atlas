#include <atomic>
#include <cstring>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "coro/pending_rpc_registry.h"
#include "coro/rpc_call.h"
#include "coro/task.h"
#include "network/channel.h"
#include "network/event_dispatcher.h"
#include "network/interface_table.h"
#include "network/message_ids.h"
#include "serialization/binary_stream.h"

using namespace atlas;

namespace atlas {
struct PendingRpcRegistryTestAccess {
  static void Age(PendingRpcRegistry& registry, PendingRpcRegistry::PendingHandle handle,
                  Duration age) {
    registry.pending_.at(registry.handles_.at(handle.value)).started_at = Clock::now() - age;
  }
  static void ExhaustAfterNext(PendingRpcRegistry& registry) {
    registry.next_request_id_ = UINT64_MAX;
  }
};
}  // namespace atlas

static auto make_payload(uint32_t request_id) -> std::vector<std::byte> {
  std::vector<std::byte> buf(sizeof(uint32_t));
  std::memcpy(buf.data(), &request_id, sizeof(uint32_t));
  return buf;
}

class PendingRpcRegistryTest : public ::testing::Test {
 protected:
  EventDispatcher dispatcher_{"test"};

  void SetUp() override { dispatcher_.SetMaxPollWait(Milliseconds(1)); }

  void drive_until(const std::atomic<bool>& done, Milliseconds timeout = Milliseconds(2000)) {
    auto deadline = Clock::now() + timeout;
    while (!done.load() && Clock::now() < deadline) dispatcher_.ProcessOnce();
  }
};

TEST_F(PendingRpcRegistryTest, OldestPendingAgeTracksOnlyLiveWaiters) {
  PendingRpcRegistry registry(dispatcher_);
  EXPECT_EQ(registry.OldestPendingAge(), Duration::zero());
  auto first = registry.RegisterPending(
      42, 1, [](std::span<const std::byte>) {}, [](Error) {}, Duration::zero());
  auto second = registry.RegisterPending(
      42, 2, [](std::span<const std::byte>) {}, [](Error) {}, Duration::zero());
  PendingRpcRegistryTestAccess::Age(registry, first, Milliseconds(5000));
  PendingRpcRegistryTestAccess::Age(registry, second, Milliseconds(1000));
  EXPECT_GE(registry.OldestPendingAge(), Milliseconds(5000));
  registry.Cancel(first);
  EXPECT_GE(registry.OldestPendingAge(), Milliseconds(1000));
  EXPECT_LT(registry.OldestPendingAge(), Milliseconds(5000));
  EXPECT_TRUE(registry.TryDispatch(42, make_payload(2)));
  EXPECT_EQ(registry.OldestPendingAge(), Duration::zero());
  registry.Shutdown();
  EXPECT_EQ(registry.OldestPendingAge(), Duration::zero());
}

TEST_F(PendingRpcRegistryTest, NativeRequestIdsRemainMonotoneAcrossCleanupAndRejectExhaustion) {
  PendingRpcRegistry registry(dispatcher_);
  EXPECT_EQ(registry.AllocateRequestId(), 1u);
  registry.CancelAll();
  EXPECT_EQ(registry.AllocateRequestId(), 2u);
  PendingRpcRegistryTestAccess::ExhaustAfterNext(registry);
  EXPECT_EQ(registry.AllocateRequestId(), UINT64_MAX);
  EXPECT_EQ(registry.AllocateRequestId(), 0u);
  EXPECT_EQ(registry.AllocateRequestId(), 0u);
}

TEST_F(PendingRpcRegistryTest, PendingLimitRejectsNewWaiterAndReleasesCapacityOnCompletion) {
  PendingRpcRegistry registry(dispatcher_);
  for (uint32_t id = 1; id <= PendingRpcRegistry::kMaxPendingRpcs; ++id) {
    ASSERT_TRUE(registry
                    .RegisterPending(
                        42, id, [](std::span<const std::byte>) {}, [](Error) {}, Duration::zero())
                    .IsValid());
  }
  EXPECT_FALSE(registry
                   .RegisterPending(
                       42, 5000, [](std::span<const std::byte>) {}, [](Error) { ADD_FAILURE(); },
                       Duration::zero())
                   .IsValid());
  EXPECT_EQ(registry.PendingCount(), PendingRpcRegistry::kMaxPendingRpcs);
  EXPECT_TRUE(registry.TryDispatch(42, make_payload(1)));
  EXPECT_TRUE(registry
                  .RegisterPending(
                      42, 5000, [](std::span<const std::byte>) {}, [](Error) {}, Duration::zero())
                  .IsValid());
  registry.Shutdown();
  EXPECT_EQ(registry.PendingCount(), 0u);
}

TEST_F(PendingRpcRegistryTest, ReplyLimitRejectsNewContextAndReturnsCapacityAfterCompletion) {
  PendingRpcRegistry registry(dispatcher_);
  intptr_t first = 0;
  for (uint64_t id = 1; id <= PendingRpcRegistry::kMaxReplyContexts; ++id) {
    auto reply = registry.RegisterReply(7, id, nullptr);
    ASSERT_NE(reply, 0);
    if (id == 1) first = reply;
  }
  EXPECT_EQ(registry.RegisterReply(7, 5000, nullptr), 0);
  EXPECT_EQ(registry.ReplyCount(), PendingRpcRegistry::kMaxReplyContexts);
  registry.SendReply(first, 1, 0, {});
  EXPECT_NE(registry.RegisterReply(7, 5000, nullptr), 0);
  registry.CancelRepliesForEntity(7);
  EXPECT_EQ(registry.ReplyCount(), 0u);
}

TEST_F(PendingRpcRegistryTest, ShutdownPermanentlyRejectsReentrantAndSubsequentWork) {
  PendingRpcRegistry registry(dispatcher_);
  registry.RegisterPending(
      42, 1, [](std::span<const std::byte>) {},
      [&](Error) {
        EXPECT_EQ(registry.AllocateRequestId(), 0u);
        EXPECT_EQ(registry.RegisterReply(7, 2, nullptr), 0);
      },
      Duration::zero());
  registry.RegisterReply(7, 1, nullptr);
  registry.Shutdown();
  registry.CancelAll();
  EXPECT_TRUE(registry.IsStopping());
  EXPECT_EQ(registry.AllocateRequestId(), 0u);
  EXPECT_EQ(registry.RegisterReply(7, 3, nullptr), 0);
  EXPECT_FALSE(registry
                   .RegisterPending(
                       42, 3, [](std::span<const std::byte>) {}, [](Error) {}, Duration::zero())
                   .IsValid());
  EXPECT_EQ(registry.PendingCount(), 0u);
  EXPECT_EQ(registry.ReplyCount(), 0u);
}

namespace {

struct TestRpcMessage {
  uint32_t request_id{0};
  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{42,
                                   "TestRpc",
                                   MessageLengthStyle::kVariable,
                                   -1,
                                   MessageReliability::kReliable,
                                   MessageUrgency::kImmediate};
    return kDesc;
  }
  void Serialize(BinaryWriter& writer) const { writer.Write(request_id); }
  static auto Deserialize(BinaryReader& reader) -> Result<TestRpcMessage> {
    auto id = reader.Read<uint32_t>();
    if (!id) return id.Error();
    return TestRpcMessage{*id};
  }
};

class RpcTestChannel final : public Channel {
 public:
  RpcTestChannel(EventDispatcher& dispatcher, InterfaceTable& table)
      : Channel(dispatcher, table, Address(0x7F000001, 31000)) {}
  auto Fd() const -> FdHandle override { return kInvalidFd; }
  int sends{0};
  std::function<void()> on_send;

 private:
  auto DoSend(std::span<const std::byte> data) -> Result<size_t> override {
    ++sends;
    if (on_send) on_send();
    return data.size();
  }
};

auto AwaitTestRpc(PendingRpcRegistry& registry, Channel& channel, Result<TestRpcMessage>& result)
    -> Task<void> {
  result =
      co_await RpcCall<TestRpcMessage>(registry, channel, TestRpcMessage{100}, Milliseconds(5000));
}

}  // namespace

TEST_F(PendingRpcRegistryTest, DestroyingSuspendedCallerRemovesPendingWaiter) {
  PendingRpcRegistry registry(dispatcher_);
  InterfaceTable table;
  RpcTestChannel channel(dispatcher_, table);
  Result<TestRpcMessage> result{Error{ErrorCode::kInternalError, "not completed"}};
  {
    auto task = AwaitTestRpc(registry, channel, result);
    task.operator co_await().await_suspend(std::noop_coroutine()).resume();
    ASSERT_EQ(registry.PendingCount(), 1u);
  }
  EXPECT_EQ(registry.PendingCount(), 0u);
  EXPECT_FALSE(registry.TryDispatch(42, make_payload(100), &channel));
}

TEST_F(PendingRpcRegistryTest, RpcCallReportsOverloadAndStoppingWithoutSending) {
  PendingRpcRegistry registry(dispatcher_);
  InterfaceTable table;
  RpcTestChannel channel(dispatcher_, table);
  for (uint64_t id = 1000; id < 1000 + PendingRpcRegistry::kMaxPendingRpcs; ++id) {
    ASSERT_TRUE(registry
                    .RegisterPending(
                        42, id, [](std::span<const std::byte>) {}, [](Error) {}, Duration::zero())
                    .IsValid());
  }
  Result<TestRpcMessage> result{Error{ErrorCode::kInternalError}};
  auto overloaded = AwaitTestRpc(registry, channel, result);
  overloaded.operator co_await().await_suspend(std::noop_coroutine()).resume();
  ASSERT_FALSE(result);
  EXPECT_EQ(result.Error().Code(), ErrorCode::kRateLimited);
  EXPECT_EQ(channel.sends, 0);
  registry.Shutdown();
  auto stopped = AwaitTestRpc(registry, channel, result);
  stopped.operator co_await().await_suspend(std::noop_coroutine()).resume();
  ASSERT_FALSE(result);
  EXPECT_EQ(result.Error().Code(), ErrorCode::kReceiverGone);
  EXPECT_EQ(channel.sends, 0);
}

TEST_F(PendingRpcRegistryTest, LocalReplyHandleCompletesOnceAndChecksRequestId) {
  PendingRpcRegistry registry(dispatcher_);
  const auto reply_id = msg_id::Id(msg_id::Common::kEntityRpcReply);
  int replies = 0;
  registry.RegisterPending(
      reply_id, 99, [&](std::span<const std::byte>) { ++replies; }, [](Error) { ADD_FAILURE(); },
      Milliseconds(5000));
  auto reply = registry.RegisterReply(7, 99, nullptr);
  ASSERT_NE(reply, 0);
  registry.SendReply(reply, 98, 0, {});
  EXPECT_EQ(registry.ReplyCount(), 1u);
  registry.SendReply(reply, 99, 0, {});
  registry.SendReply(reply, 99, 0, {});
  EXPECT_EQ(replies, 1);
  EXPECT_EQ(registry.ReplyCount(), 0u);
}

TEST_F(PendingRpcRegistryTest, EntityDestroyInvalidatesReplyHandleAndCompletesLocalWaiter) {
  PendingRpcRegistry registry(dispatcher_);
  int error_code = 0;
  const auto reply_id = msg_id::Id(msg_id::Common::kEntityRpcReply);
  registry.RegisterPending(
      reply_id, 99,
      [&](std::span<const std::byte> payload) {
        BinaryReader reader(payload);
        ASSERT_TRUE(reader.Read<uint64_t>());
        auto code = reader.Read<int32_t>();
        ASSERT_TRUE(code);
        error_code = *code;
      },
      [](Error) { ADD_FAILURE(); }, Milliseconds(5000));
  auto reply = registry.RegisterReply(7, 99, nullptr);
  registry.CancelRepliesForEntity(7);
  registry.SendReply(reply, 99, 0, {});
  EXPECT_EQ(error_code, -4);
  EXPECT_EQ(registry.ReplyCount(), 0u);
  EXPECT_EQ(registry.PendingCount(), 0u);
}

TEST_F(PendingRpcRegistryTest, DisconnectInvalidatesReplyBeforeChannelIsReleased) {
  PendingRpcRegistry registry(dispatcher_);
  InterfaceTable table;
  auto channel = make_intrusive<RpcTestChannel>(dispatcher_, table);
  auto* raw = channel.get();
  auto reply = registry.RegisterReply(7, 99, raw);
  ASSERT_NE(reply, 0);
  channel.reset();
  registry.CancelByChannel(raw);
  EXPECT_EQ(registry.ReplyCount(), 0u);
  registry.SendReply(reply, 99, 0, {});
  auto next = registry.RegisterReply(7, 100, nullptr);
  EXPECT_NE(reply, next);
  registry.SendReply(reply, 99, 0, {});
  EXPECT_EQ(registry.ReplyCount(), 1u);
  registry.CancelAll();
  EXPECT_EQ(registry.ReplyCount(), 0u);
}

TEST_F(PendingRpcRegistryTest, RpcCallRejectsDuplicateBeforeSending) {
  PendingRpcRegistry registry(dispatcher_);
  InterfaceTable table;
  RpcTestChannel channel(dispatcher_, table);
  registry.RegisterPending(
      42, 100, [](std::span<const std::byte>) {}, [](Error) {}, Milliseconds(5000), &channel);
  Result<TestRpcMessage> result{Error{ErrorCode::kInternalError, "not completed"}};
  auto task = AwaitTestRpc(registry, channel, result);
  task.operator co_await().await_suspend(std::noop_coroutine()).resume();
  ASSERT_FALSE(result);
  EXPECT_EQ(result.Error().Code(), ErrorCode::kAlreadyExists);
  EXPECT_EQ(channel.sends, 0);
  EXPECT_EQ(registry.PendingCount(), 1u);
}

TEST_F(PendingRpcRegistryTest, RpcCallCanCompleteSynchronouslyDuringSend) {
  PendingRpcRegistry registry(dispatcher_);
  InterfaceTable table;
  RpcTestChannel channel(dispatcher_, table);
  channel.on_send = [&] { EXPECT_TRUE(registry.TryDispatch(42, make_payload(100), &channel)); };
  Result<TestRpcMessage> result{Error{ErrorCode::kInternalError, "not completed"}};
  auto task = AwaitTestRpc(registry, channel, result);
  task.operator co_await().await_suspend(std::noop_coroutine()).resume();
  ASSERT_TRUE(result);
  EXPECT_EQ(result->request_id, 100u);
  EXPECT_EQ(registry.PendingCount(), 0u);
}

TEST_F(PendingRpcRegistryTest, RpcCallSendFailureLeavesNoPendingWaiter) {
  PendingRpcRegistry registry(dispatcher_);
  InterfaceTable table;
  RpcTestChannel channel(dispatcher_, table);
  channel.Condemn();
  Result<TestRpcMessage> result{Error{ErrorCode::kInternalError, "not completed"}};
  auto task = AwaitTestRpc(registry, channel, result);
  task.operator co_await().await_suspend(std::noop_coroutine()).resume();
  ASSERT_FALSE(result);
  EXPECT_EQ(result.Error().Code(), ErrorCode::kChannelCondemned);
  EXPECT_EQ(registry.PendingCount(), 0u);
}

TEST_F(PendingRpcRegistryTest, DuplicateRegistrationPreservesOriginalWaiter) {
  PendingRpcRegistry registry(dispatcher_);
  int replies = 0;
  auto original = registry.RegisterPending(
      42, 100, [&](std::span<const std::byte>) { ++replies; }, [](Error) {}, Milliseconds(5000));
  auto duplicate = registry.RegisterPending(
      42, 100, [](std::span<const std::byte>) { ADD_FAILURE() << "duplicate consumed reply"; },
      [](Error) {}, Milliseconds(5000));
  EXPECT_TRUE(original.IsValid());
  EXPECT_FALSE(duplicate.IsValid());
  EXPECT_EQ(registry.PendingCount(), 1u);
  EXPECT_TRUE(registry.TryDispatch(42, make_payload(100)));
  EXPECT_EQ(replies, 1);
}

TEST_F(PendingRpcRegistryTest, CancelAllRejectsReentrantRegistration) {
  PendingRpcRegistry registry(dispatcher_);
  PendingRpcRegistry::PendingHandle reentrant;
  registry.RegisterPending(
      42, 100, [](std::span<const std::byte>) {},
      [&](Error) {
        reentrant = registry.RegisterPending(
            42, 101, [](std::span<const std::byte>) {}, [](Error) {}, Milliseconds(5000));
      },
      Milliseconds(5000));
  registry.CancelAll();
  EXPECT_FALSE(reentrant.IsValid());
  EXPECT_EQ(registry.PendingCount(), 0u);
}

TEST_F(PendingRpcRegistryTest, OnlyRegisteredChannelCanCompleteReply) {
  PendingRpcRegistry registry(dispatcher_);
  auto* expected = reinterpret_cast<Channel*>(0xA1A1);
  auto* forged = reinterpret_cast<Channel*>(0xB2B2);
  int replies = 0;
  registry.RegisterPending(
      42, 100, [&](std::span<const std::byte>) { ++replies; }, [](Error) {}, Milliseconds(5000),
      expected);
  auto payload = make_payload(100);
  EXPECT_FALSE(registry.TryDispatch(42, payload, forged));
  EXPECT_FALSE(registry.TryDispatch(42, payload));
  EXPECT_EQ(registry.PendingCount(), 1u);
  EXPECT_TRUE(registry.TryDispatch(42, payload, expected));
  EXPECT_FALSE(registry.TryDispatch(42, payload, expected));
  EXPECT_EQ(replies, 1);
}

TEST_F(PendingRpcRegistryTest, RemoteChannelCannotCompleteLocalWaiter) {
  PendingRpcRegistry registry(dispatcher_);
  int replies = 0;
  registry.RegisterPending(
      42, 100, [&](std::span<const std::byte>) { ++replies; }, [](Error) {}, Milliseconds(5000));
  auto payload = make_payload(100);
  EXPECT_FALSE(registry.TryDispatch(42, payload, reinterpret_cast<Channel*>(0xA1A1)));
  EXPECT_TRUE(registry.TryDispatch(42, payload));
  EXPECT_EQ(replies, 1);
}

TEST_F(PendingRpcRegistryTest, AbandonRemovesWaiterWithoutResumingDestroyedCaller) {
  PendingRpcRegistry registry(dispatcher_);
  auto handle = registry.RegisterPending(
      42, 100, [](std::span<const std::byte>) { ADD_FAILURE(); }, [](Error) { ADD_FAILURE(); },
      Milliseconds(5000));
  registry.Abandon(handle);
  registry.Cancel(handle);
  EXPECT_FALSE(registry.TryDispatch(42, make_payload(100)));
  EXPECT_EQ(registry.PendingCount(), 0u);
}

TEST_F(PendingRpcRegistryTest, CancelByChannelFiresReceiverGone) {
  PendingRpcRegistry registry(dispatcher_);

  // Use distinct sentinel pointers — registry treats them as opaque keys.
  auto* alive = reinterpret_cast<Channel*>(0xA1A1);
  auto* doomed = reinterpret_cast<Channel*>(0xB2B2);

  std::vector<ErrorCode> alive_errors;
  std::vector<ErrorCode> doomed_errors;
  bool alive_replied = false;

  registry.RegisterPending(
      10, 1, [&](std::span<const std::byte>) { alive_replied = true; },
      [&](Error e) { alive_errors.push_back(e.Code()); }, Milliseconds(60'000), alive);
  registry.RegisterPending(
      10, 2, [](std::span<const std::byte>) {}, [&](Error e) { doomed_errors.push_back(e.Code()); },
      Milliseconds(60'000), doomed);
  registry.RegisterPending(
      10, 3, [](std::span<const std::byte>) {}, [&](Error e) { doomed_errors.push_back(e.Code()); },
      Milliseconds(60'000), doomed);

  EXPECT_EQ(registry.PendingCount(), 3u);
  registry.CancelByChannel(doomed);
  EXPECT_EQ(registry.PendingCount(), 1u);
  EXPECT_EQ(doomed_errors.size(), 2u);
  EXPECT_EQ(doomed_errors[0], ErrorCode::kReceiverGone);
  EXPECT_EQ(doomed_errors[1], ErrorCode::kReceiverGone);
  EXPECT_TRUE(alive_errors.empty());

  auto payload = make_payload(1);
  EXPECT_TRUE(registry.TryDispatch(10, payload, alive));
  EXPECT_TRUE(alive_replied);
}

TEST_F(PendingRpcRegistryTest, CancelByChannelNullIgnored) {
  PendingRpcRegistry registry(dispatcher_);
  registry.RegisterPending(
      10, 1, [](std::span<const std::byte>) {}, [](Error) {}, Milliseconds(60'000));
  registry.CancelByChannel(nullptr);
  EXPECT_EQ(registry.PendingCount(), 1u);
}

TEST_F(PendingRpcRegistryTest, BasicDispatch) {
  PendingRpcRegistry registry(dispatcher_);

  bool reply_received = false;
  registry.RegisterPending(
      42,   // reply message ID
      100,  // request_id
      [&](std::span<const std::byte>) { reply_received = true; }, [&](Error) {},
      Milliseconds(5000));

  EXPECT_EQ(registry.PendingCount(), 1u);

  auto payload = make_payload(100);
  bool consumed = registry.TryDispatch(42, payload);

  EXPECT_TRUE(consumed);
  EXPECT_TRUE(reply_received);
  EXPECT_EQ(registry.PendingCount(), 0u);
}

TEST_F(PendingRpcRegistryTest, NoMatchReturnsfalse) {
  PendingRpcRegistry registry(dispatcher_);

  registry.RegisterPending(
      42, 100, [](std::span<const std::byte>) {}, [](Error) {}, Milliseconds(5000));

  // Wrong message ID
  auto payload = make_payload(100);
  EXPECT_FALSE(registry.TryDispatch(99, payload));

  // Wrong request_id
  auto payload2 = make_payload(999);
  EXPECT_FALSE(registry.TryDispatch(42, payload2));

  EXPECT_EQ(registry.PendingCount(), 1u);
}

TEST_F(PendingRpcRegistryTest, Timeout) {
  PendingRpcRegistry registry(dispatcher_);

  std::atomic<bool> timed_out{false};
  ErrorCode error_code = ErrorCode::kNone;

  registry.RegisterPending(
      42, 100, [](std::span<const std::byte>) {},
      [&](Error err) {
        error_code = err.Code();
        timed_out = true;
      },
      Milliseconds(20));

  drive_until(timed_out, Milliseconds(500));

  EXPECT_TRUE(timed_out.load());
  EXPECT_EQ(error_code, ErrorCode::kTimeout);
  EXPECT_EQ(registry.PendingCount(), 0u);
}

TEST_F(PendingRpcRegistryTest, ManualCancel) {
  PendingRpcRegistry registry(dispatcher_);

  ErrorCode error_code = ErrorCode::kNone;
  auto handle = registry.RegisterPending(
      42, 100, [](std::span<const std::byte>) {}, [&](Error err) { error_code = err.Code(); },
      Milliseconds(5000));

  registry.Cancel(handle);

  EXPECT_EQ(error_code, ErrorCode::kCancelled);
  EXPECT_EQ(registry.PendingCount(), 0u);
}

TEST_F(PendingRpcRegistryTest, CancelAll) {
  PendingRpcRegistry registry(dispatcher_);

  int cancel_count = 0;
  for (uint32_t i = 0; i < 5; ++i) {
    registry.RegisterPending(
        42, i, [](std::span<const std::byte>) {}, [&](Error) { ++cancel_count; },
        Milliseconds(5000));
  }

  EXPECT_EQ(registry.PendingCount(), 5u);
  registry.CancelAll();
  EXPECT_EQ(cancel_count, 5);
  EXPECT_EQ(registry.PendingCount(), 0u);
}

TEST_F(PendingRpcRegistryTest, SessionRepliesMatchFullWidthIds) {
  for (const auto reply_id : {msg_id::Id(msg_id::DBApp::kAccountSessionReply),
                              msg_id::Id(msg_id::BaseApp::kRevokeAccountSessionReply),
                              msg_id::Id(msg_id::BaseApp::kPrepareClientHandoffReply)}) {
    PendingRpcRegistry registry(dispatcher_);
    constexpr uint64_t kLow = 7;
    constexpr uint64_t kHigh = (uint64_t{1} << 40) + kLow;
    bool low = false;
    bool high = false;
    registry.RegisterPending(
        reply_id, kLow, [&](std::span<const std::byte>) { low = true; }, [](Error) {},
        Milliseconds(5000));
    registry.RegisterPending(
        reply_id, kHigh, [&](std::span<const std::byte>) { high = true; }, [](Error) {},
        Milliseconds(5000));
    BinaryWriter truncated;
    truncated.Write(static_cast<uint32_t>(kLow));
    EXPECT_FALSE(registry.TryDispatch(reply_id, truncated.Data()));
    BinaryWriter first;
    first.Write(kHigh);
    EXPECT_TRUE(registry.TryDispatch(reply_id, first.Data()));
    EXPECT_TRUE(high);
    EXPECT_FALSE(low);
    EXPECT_FALSE(registry.TryDispatch(reply_id, first.Data()));
    BinaryWriter second;
    second.Write(kLow);
    EXPECT_TRUE(registry.TryDispatch(reply_id, second.Data()));
    EXPECT_TRUE(low);
    EXPECT_EQ(registry.PendingCount(), 0u);
  }
}

TEST_F(PendingRpcRegistryTest, PayloadTooSmallNotConsumed) {
  PendingRpcRegistry registry(dispatcher_);

  registry.RegisterPending(
      42, 100, [](std::span<const std::byte>) {}, [](Error) {}, Milliseconds(5000));

  // Payload smaller than 4 bytes — cannot extract request_id
  std::vector<std::byte> tiny(2);
  EXPECT_FALSE(registry.TryDispatch(42, tiny));
  EXPECT_EQ(registry.PendingCount(), 1u);
}

TEST(InterfaceTable, PreDispatchHookInterceptsMessage) {
  InterfaceTable table;

  bool hook_called = false;
  MessageID intercepted_id = 0;
  auto* channel = reinterpret_cast<Channel*>(0xA1A1);

  table.SetPreDispatchHook([&](Channel* source, MessageID id, std::span<const std::byte>) -> bool {
    hook_called = true;
    intercepted_id = id;
    EXPECT_EQ(source, channel);
    return true;  // consume the message
  });

  // Build a fake message
  BinaryWriter writer;
  writer.Write<uint32_t>(12345);  // request_id
  BinaryReader reader(writer.Data());

  Address addr;
  auto result = table.Dispatch(addr, channel, 42, reader);

  EXPECT_TRUE(result.HasValue());
  EXPECT_TRUE(hook_called);
  EXPECT_EQ(intercepted_id, 42);
}

TEST(InterfaceTable, PreDispatchHookPassThrough) {
  InterfaceTable table;

  table.SetPreDispatchHook([](Channel*, MessageID, std::span<const std::byte>) -> bool {
    return false;  // do not consume
  });

  BinaryWriter writer;
  writer.Write<uint32_t>(0);
  BinaryReader reader(writer.Data());

  Address addr;
  // No handler registered for this ID — should fall through to NotFound
  auto result = table.Dispatch(addr, nullptr, 42, reader);
  EXPECT_FALSE(result.HasValue());
  EXPECT_EQ(result.Error().Code(), ErrorCode::kNotFound);
}
