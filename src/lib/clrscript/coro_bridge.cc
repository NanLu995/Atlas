#include "clrscript/coro_bridge.h"

#include <chrono>

#include "foundation/error.h"
#include "network/message_ids.h"
#include "serialization/binary_stream.h"

namespace atlas::coro_bridge {

namespace {

constexpr int32_t kStatusSuccess = 0;
constexpr int32_t kStatusTimeout = 1;
constexpr int32_t kStatusCancelled = 2;
constexpr int32_t kStatusSendError = 3;
constexpr int32_t kStatusReceiverGone = 4;

}  // namespace

auto RegisterPending(PendingRpcRegistry& registry, CoroOnRpcCompleteFn on_complete,
                     uint16_t reply_id, uint64_t request_id, int32_t timeout_ms,
                     intptr_t managed_handle) -> uint64_t {
  if (on_complete == nullptr) return 0;

  auto timeout = timeout_ms > 0 ? std::chrono::milliseconds{timeout_ms} : Duration::zero();

  auto handle = registry.RegisterPending(
      reply_id, request_id,
      [on_complete, managed_handle](std::span<const std::byte> payload) {
        on_complete(managed_handle, kStatusSuccess,
                    reinterpret_cast<const uint8_t*>(payload.data()),
                    static_cast<int32_t>(payload.size()));
      },
      [on_complete, managed_handle](Error err) {
        int32_t status = kStatusSendError;
        if (err.Code() == ErrorCode::kTimeout)
          status = kStatusTimeout;
        else if (err.Code() == ErrorCode::kCancelled)
          status = kStatusCancelled;
        else if (err.Code() == ErrorCode::kReceiverGone)
          status = kStatusReceiverGone;
        on_complete(managed_handle, status, nullptr, 0);
      },
      timeout);

  return handle.value;
}

void CancelPending(PendingRpcRegistry& registry, uint64_t handle) {
  if (handle == 0) return;
  registry.Cancel(PendingRpcRegistry::PendingHandle{handle});
}

auto BindEntityRequest(PendingRpcRegistry& registry, uint32_t rpc_id,
                       std::span<const std::byte> payload, Channel* channel) -> bool {
  if ((rpc_id & 0x80000000u) == 0) return true;
  BinaryReader reader(payload);
  auto request_id = reader.Read<uint64_t>();
  return request_id &&
         registry.BindChannel(msg_id::Id(msg_id::Common::kEntityRpcReply), *request_id, channel);
}

void FailEntityRequest(PendingRpcRegistry& registry, uint32_t rpc_id,
                       std::span<const std::byte> payload, Error error) {
  if ((rpc_id & 0x80000000u) == 0) return;
  BinaryReader reader(payload);
  auto request_id = reader.Read<uint64_t>();
  if (request_id)
    registry.FailPending(msg_id::Id(msg_id::Common::kEntityRpcReply), *request_id,
                         std::move(error));
}

}  // namespace atlas::coro_bridge
