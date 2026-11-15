#ifndef ATLAS_LIB_CORO_PENDING_RPC_REGISTRY_H_
#define ATLAS_LIB_CORO_PENDING_RPC_REGISTRY_H_

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <span>
#include <unordered_map>

#include "foundation/clock.h"
#include "foundation/error.h"
#include "foundation/intrusive_ptr.h"
#include "foundation/timer_queue.h"
#include "network/channel.h"
#include "network/event_dispatcher.h"
#include "network/message.h"

namespace atlas {

class Channel;

class PendingRpcRegistry {
 public:
  static constexpr size_t kMaxPendingRpcs = 4096;
  static constexpr size_t kMaxReplyContexts = 4096;
  explicit PendingRpcRegistry(EventDispatcher& dispatcher);
  auto AllocateRequestId() -> uint64_t;
  ~PendingRpcRegistry();

  PendingRpcRegistry(const PendingRpcRegistry&) = delete;
  PendingRpcRegistry& operator=(const PendingRpcRegistry&) = delete;

  using ReplyCallback = std::function<void(std::span<const std::byte> payload)>;
  using ErrorCallback = std::function<void(Error error)>;

  struct PendingHandle {
    uint64_t value{0};
    [[nodiscard]] auto IsValid() const -> bool { return value != 0; }
  };

  // Rejection returns an invalid handle without invoking either callback.
  auto RegisterPending(MessageID reply_id, uint64_t request_id, ReplyCallback on_reply,
                       ErrorCallback on_error, Duration timeout, Channel* channel = nullptr,
                       Error* rejection = nullptr) -> PendingHandle;

  // Entity and account-session replies use uint64_t IDs; older control replies use uint32_t.
  auto TryDispatch(MessageID id, std::span<const std::byte> payload, Channel* source = nullptr)
      -> bool;

  void Cancel(PendingHandle handle);
  void Abandon(PendingHandle handle);
  auto BindChannel(MessageID reply_id, uint64_t request_id, Channel* channel) -> bool;
  void FailPending(MessageID reply_id, uint64_t request_id, Error error);
  auto RegisterReply(uint32_t entity_id, uint64_t request_id, Channel* channel) -> intptr_t;
  void SendReply(intptr_t handle, uint64_t request_id, int32_t error_code,
                 std::span<const std::byte> body);
  void CancelRepliesForEntity(uint32_t entity_id);
  void BindReplyWait(intptr_t reply, PendingHandle pending);
  [[nodiscard]] auto ReplyCount() const -> size_t { return replies_.size(); }

  // Fires kReceiverGone on every pending registered against the given channel.
  void CancelByChannel(Channel* channel);

  void CancelAll();
  void Shutdown();
  [[nodiscard]] auto IsStopping() const -> bool { return stopping_; }

  [[nodiscard]] auto PendingCount() const -> size_t;

 private:
  friend struct PendingRpcRegistryTestAccess;
  struct PendingKey {
    MessageID reply_id;
    uint64_t request_id;
    auto operator==(const PendingKey&) const -> bool = default;
  };

  struct PendingKeyHash {
    auto operator()(const PendingKey& k) const -> size_t {
      auto h = static_cast<size_t>(k.reply_id);
      h ^= std::hash<uint64_t>{}(k.request_id) + 0x9e3779b9 + (h << 6) + (h >> 2);
      return h;
    }
  };

  struct PendingEntry {
    ReplyCallback on_reply;
    ErrorCallback on_error;
    TimerHandle timeout_timer;
    Channel* channel{nullptr};
    PendingHandle handle;
  };

  static auto ExtractRequestId(MessageID id, std::span<const std::byte> payload) -> uint64_t;
  void CompleteError(PendingHandle handle, Error error);

  std::unordered_map<PendingKey, PendingEntry, PendingKeyHash> pending_;
  std::unordered_map<uint64_t, PendingKey> handles_;
  struct ReplyEntry {
    uint32_t entity_id;
    uint64_t request_id;
    IntrusivePtr<Channel> channel;
    PendingHandle pending;
  };
  std::unordered_map<intptr_t, ReplyEntry> replies_;
  uint64_t next_handle_{1};
  uint64_t next_request_id_{1};
  bool cancelling_all_{false};
  bool stopping_{false};
  EventDispatcher& dispatcher_;
};

}  // namespace atlas

#endif  // ATLAS_LIB_CORO_PENDING_RPC_REGISTRY_H_
