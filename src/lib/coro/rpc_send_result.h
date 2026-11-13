#ifndef ATLAS_LIB_CORO_RPC_SEND_RESULT_H_
#define ATLAS_LIB_CORO_RPC_SEND_RESULT_H_

#include <cstdint>

#include "foundation/error.h"

namespace atlas {

enum class RpcSendResult : int32_t {
  kQueued = 0,
  kInvalidTarget = 1,
  kOverloaded = 2,
  kStopping = 3,
  kSendFailed = 4,
  kPartiallyQueued = 5,
};

inline auto RpcSendOutcome(const Result<void>& sent) -> RpcSendResult {
  if (sent) return RpcSendResult::kQueued;
  switch (sent.Error().Code()) {
    case ErrorCode::kWouldBlock:
    case ErrorCode::kRateLimited:
      return RpcSendResult::kOverloaded;
    case ErrorCode::kChannelCondemned:
    case ErrorCode::kReceiverGone:
    case ErrorCode::kNotFound:
      return RpcSendResult::kInvalidTarget;
    default:
      return RpcSendResult::kSendFailed;
  }
}

}  // namespace atlas

#endif  // ATLAS_LIB_CORO_RPC_SEND_RESULT_H_
