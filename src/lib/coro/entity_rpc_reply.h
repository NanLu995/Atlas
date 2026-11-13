#ifndef ATLAS_LIB_CORO_ENTITY_RPC_REPLY_H_
#define ATLAS_LIB_CORO_ENTITY_RPC_REPLY_H_

#include <cstdint>
#include <span>
#include <string_view>

#include "foundation/error.h"
#include "network/channel.h"
#include "network/message.h"
#include "network/message_ids.h"
#include "serialization/binary_stream.h"

namespace atlas {

// Reply body is user data on success or a VLE-prefixed error string on failure.
struct EntityRpcReply {
  uint64_t request_id{0};
  int32_t error_code{0};
  std::span<const std::byte> body;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::Common::kEntityRpcReply),
                                   "Common::EntityRpcReply",
                                   MessageLengthStyle::kVariable,
                                   -1,
                                   MessageReliability::kReliable,
                                   MessageUrgency::kImmediate};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write(request_id);
    w.Write(error_code);
    w.WriteBytes(body);
  }

  // Send-only — replies are claimed by PendingRpcRegistry via the wire-level
  // pre-dispatch hook, never typed-dispatched.
  static auto Deserialize(BinaryReader&) -> Result<EntityRpcReply> {
    return Error{ErrorCode::kInvalidArgument, "EntityRpcReply is send-only"};
  }
};
static_assert(NetworkMessage<EntityRpcReply>);

namespace entity_rpc_reply {

// Builds the body for an error reply: [vle-string error_msg].
inline void WriteErrorBody(BinaryWriter& body, std::string_view error_msg) {
  body.WriteString(error_msg);
}

inline auto SendSuccess(Channel& ch, uint64_t request_id, std::span<const std::byte> body)
    -> Result<void> {
  return ch.SendMessage(EntityRpcReply{
      .request_id = request_id,
      .error_code = 0,
      .body = body,
  });
}

inline auto SendFailure(Channel& ch, uint64_t request_id, int32_t error_code,
                        std::string_view error_msg) -> Result<void> {
  BinaryWriter body;
  WriteErrorBody(body, error_msg);
  return ch.SendMessage(EntityRpcReply{
      .request_id = request_id,
      .error_code = error_code,
      .body = body.Data(),
  });
}

inline void RejectRequest(Channel* channel, uint32_t rpc_id, std::span<const std::byte> payload,
                          int32_t error_code, std::string_view message) {
  if (!channel || (rpc_id & 0x80000000u) == 0) return;
  BinaryReader reader(payload);
  auto request_id = reader.Read<uint64_t>();
  if (request_id && *request_id != 0) (void)SendFailure(*channel, *request_id, error_code, message);
}

}  // namespace entity_rpc_reply

}  // namespace atlas

#endif  // ATLAS_LIB_CORO_ENTITY_RPC_REPLY_H_
