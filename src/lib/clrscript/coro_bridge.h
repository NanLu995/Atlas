#ifndef ATLAS_LIB_CLRSCRIPT_CORO_BRIDGE_H_
#define ATLAS_LIB_CLRSCRIPT_CORO_BRIDGE_H_

#include <cstddef>
#include <cstdint>

#include "coro/pending_rpc_registry.h"

namespace atlas {

// status values: 0=Success 1=Timeout 2=Cancelled 3=SendError 4=ReceiverGone. Mirrors
// Atlas.Coro.Rpc.RpcCompletionStatus.
using CoroOnRpcCompleteFn = void (*)(intptr_t managed_handle, int32_t status,
                                     const uint8_t* payload, int32_t len);

namespace coro_bridge {

// The opaque handle identifies this registration, even when a request key is reused.
// timeout_ms <= 0 means "never time out".
auto RegisterPending(PendingRpcRegistry& registry, CoroOnRpcCompleteFn on_complete,
                     uint16_t reply_id, uint64_t request_id, int32_t timeout_ms,
                     intptr_t managed_handle) -> uint64_t;

void CancelPending(PendingRpcRegistry& registry, uint64_t handle);
auto BindEntityRequest(PendingRpcRegistry& registry, uint32_t rpc_id,
                       std::span<const std::byte> payload, Channel* channel) -> bool;
void FailEntityRequest(PendingRpcRegistry& registry, uint32_t rpc_id,
                       std::span<const std::byte> payload, Error error);

}  // namespace coro_bridge

}  // namespace atlas

#endif  // ATLAS_LIB_CLRSCRIPT_CORO_BRIDGE_H_
