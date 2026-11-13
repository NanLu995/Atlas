#include "coro/pending_rpc_registry.h"

#include <limits>
#include <vector>

#include "coro/entity_rpc_reply.h"
#include "network/message_ids.h"
#include "serialization/binary_stream.h"

namespace atlas {

PendingRpcRegistry::PendingRpcRegistry(EventDispatcher& dispatcher) : dispatcher_(dispatcher) {}

auto PendingRpcRegistry::AllocateRequestId() -> uint64_t {
  if (stopping_ || cancelling_all_ || next_request_id_ == 0) return 0;
  return next_request_id_++;
}

PendingRpcRegistry::~PendingRpcRegistry() {
  Shutdown();
}

auto PendingRpcRegistry::RegisterPending(MessageID reply_id, uint64_t request_id,
                                         ReplyCallback on_reply, ErrorCallback on_error,
                                         Duration timeout, Channel* channel) -> PendingHandle {
  auto key = PendingKey{reply_id, request_id};
  if (reply_id == 0 || !on_reply || stopping_ || cancelling_all_ || next_handle_ == 0 ||
      pending_.size() >= kMaxPendingRpcs || pending_.contains(key)) {
    return {};
  }
  PendingHandle handle{next_handle_++};
  TimerHandle timer;
  if (timeout > Duration::zero()) {
    timer = dispatcher_.AddTimer(timeout, [this, handle](TimerHandle) {
      CompleteError(handle, Error{ErrorCode::kTimeout, "RPC timed out"});
    });
  }
  pending_.emplace(key,
                   PendingEntry{std::move(on_reply), std::move(on_error), timer, channel, handle});
  handles_.emplace(handle.value, key);
  return handle;
}

auto PendingRpcRegistry::TryDispatch(MessageID id, std::span<const std::byte> payload,
                                     Channel* source) -> bool {
  const bool entity_reply = id == msg_id::Id(msg_id::Common::kEntityRpcReply);
  if (payload.size() < (entity_reply ? sizeof(uint64_t) : sizeof(uint32_t))) return false;

  auto request_id = ExtractRequestId(id, payload);
  auto key = PendingKey{id, request_id};
  auto it = pending_.find(key);

  if (it == pending_.end() || it->second.channel != source) return false;

  dispatcher_.CancelTimer(it->second.timeout_timer);

  auto on_reply = std::move(it->second.on_reply);
  handles_.erase(it->second.handle.value);
  pending_.erase(it);

  on_reply(payload);
  return true;
}

void PendingRpcRegistry::Cancel(PendingHandle handle) {
  CompleteError(handle, Error{ErrorCode::kCancelled, "RPC cancelled"});
}

auto PendingRpcRegistry::BindChannel(MessageID reply_id, uint64_t request_id, Channel* channel)
    -> bool {
  auto found = pending_.find(PendingKey{reply_id, request_id});
  if (found == pending_.end() || found->second.channel != nullptr) return false;
  found->second.channel = channel;
  return true;
}

void PendingRpcRegistry::FailPending(MessageID reply_id, uint64_t request_id, Error error) {
  auto found = pending_.find(PendingKey{reply_id, request_id});
  if (found == pending_.end()) return;
  CompleteError(found->second.handle, std::move(error));
}

auto PendingRpcRegistry::RegisterReply(uint32_t entity_id, uint64_t request_id, Channel* channel)
    -> intptr_t {
  if (stopping_ || cancelling_all_ || request_id == 0 || next_handle_ == 0 ||
      replies_.size() >= kMaxReplyContexts ||
      next_handle_ > static_cast<uint64_t>((std::numeric_limits<intptr_t>::max)()) ||
      (channel && channel->IsCondemned()))
    return 0;
  const auto handle = static_cast<intptr_t>(next_handle_++);
  replies_.emplace(handle, ReplyEntry{entity_id, request_id, IntrusivePtr<Channel>{channel}, {}});
  return handle;
}

void PendingRpcRegistry::SendReply(intptr_t handle, uint64_t request_id, int32_t error_code,
                                   std::span<const std::byte> body) {
  auto found = replies_.find(handle);
  if (found == replies_.end() || found->second.request_id != request_id) return;
  auto entry = std::move(found->second);
  replies_.erase(found);
  Abandon(entry.pending);
  EntityRpcReply reply{request_id, error_code, body};
  if (entry.channel) {
    if (!entry.channel->IsCondemned()) (void)entry.channel->SendMessage(reply);
  } else {
    BinaryWriter writer;
    reply.Serialize(writer);
    (void)TryDispatch(reply.Descriptor().id, writer.Data());
  }
}

void PendingRpcRegistry::BindReplyWait(intptr_t reply, PendingHandle pending) {
  auto found = replies_.find(reply);
  if (found != replies_.end())
    found->second.pending = pending;
  else
    Abandon(pending);
}

void PendingRpcRegistry::CancelRepliesForEntity(uint32_t entity_id) {
  std::vector<intptr_t> handles;
  for (const auto& [handle, entry] : replies_) {
    if (entry.entity_id == entity_id) handles.push_back(handle);
  }
  BinaryWriter body;
  body.WriteString("RPC target entity destroyed");
  for (auto handle : handles) {
    auto found = replies_.find(handle);
    if (found != replies_.end()) SendReply(handle, found->second.request_id, -4, body.Data());
  }
}

void PendingRpcRegistry::Abandon(PendingHandle handle) {
  auto found = handles_.find(handle.value);
  if (found == handles_.end()) return;
  auto it = pending_.find(found->second);
  dispatcher_.CancelTimer(it->second.timeout_timer);
  pending_.erase(it);
  handles_.erase(found);
}

void PendingRpcRegistry::CompleteError(PendingHandle handle, Error error) {
  auto found = handles_.find(handle.value);
  if (found == handles_.end()) return;
  auto it = pending_.find(found->second);

  dispatcher_.CancelTimer(it->second.timeout_timer);

  auto error_cb = std::move(it->second.on_error);
  handles_.erase(found);
  pending_.erase(it);

  if (error_cb) error_cb(std::move(error));
}

void PendingRpcRegistry::CancelByChannel(Channel* channel) {
  if (channel == nullptr) return;
  std::erase_if(replies_, [this, channel](const auto& entry) {
    if (entry.second.channel.get() != channel) return false;
    Abandon(entry.second.pending);
    return true;
  });

  // Completion callbacks may cancel peers or register new calls on this channel.
  std::vector<PendingHandle> doomed;
  doomed.reserve(pending_.size());
  for (const auto& [key, entry] : pending_) {
    if (entry.channel == channel) doomed.push_back(entry.handle);
  }

  for (const auto& handle : doomed) {
    CompleteError(handle, Error{ErrorCode::kReceiverGone, "RPC peer channel disconnected"});
  }
}

void PendingRpcRegistry::CancelAll() {
  if (cancelling_all_) return;
  cancelling_all_ = true;
  replies_.clear();
  auto entries = std::move(pending_);
  pending_.clear();
  handles_.clear();

  for (auto& [key, entry] : entries) {
    dispatcher_.CancelTimer(entry.timeout_timer);
    if (entry.on_error) entry.on_error(Error{ErrorCode::kCancelled, "RPC registry shutting down"});
  }
  cancelling_all_ = false;
}

void PendingRpcRegistry::Shutdown() {
  stopping_ = true;
  CancelAll();
}

auto PendingRpcRegistry::PendingCount() const -> size_t {
  return pending_.size();
}

auto PendingRpcRegistry::ExtractRequestId(MessageID id, std::span<const std::byte> payload)
    -> uint64_t {
  if (id == msg_id::Id(msg_id::Common::kEntityRpcReply)) {
    uint64_t id_le = 0;
    std::memcpy(&id_le, payload.data(), sizeof(id_le));
    return endian::FromLittle(id_le);
  }
  uint32_t id_le = 0;
  std::memcpy(&id_le, payload.data(), sizeof(uint32_t));
  return endian::FromLittle(id_le);
}

}  // namespace atlas
