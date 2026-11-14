#ifndef ATLAS_SERVER_BASEAPP_SESSION_MESSAGES_H_
#define ATLAS_SERVER_BASEAPP_SESSION_MESSAGES_H_

#include "db/account_session.h"
#include "network/message.h"
#include "network/message_ids.h"

namespace atlas::baseapp {

struct RevokeAccountSession {
  uint64_t request_id{0};
  uint64_t requester_instance{0};
  DatabaseID account_dbid{kInvalidDBID};
  uint16_t account_type_id{0};
  uint64_t version{0};
  AccountSessionOwner owner;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::BaseApp::kRevokeAccountSession),
                                   "baseapp::RevokeAccountSession",
                                   MessageLengthStyle::kVariable,
                                   -1,
                                   MessageReliability::kReliable,
                                   MessageUrgency::kImmediate};
    return kDesc;
  }
  void Serialize(BinaryWriter& writer) const {
    writer.Write(request_id);
    writer.Write(requester_instance);
    writer.Write(account_dbid);
    writer.Write(account_type_id);
    writer.Write(version);
    WriteSessionOwner(writer, owner);
  }
  static auto Deserialize(BinaryReader& reader) -> Result<RevokeAccountSession> {
    auto request = reader.Read<uint64_t>();
    auto instance = reader.Read<uint64_t>();
    auto dbid = reader.Read<DatabaseID>();
    auto type = reader.Read<uint16_t>();
    auto version = reader.Read<uint64_t>();
    auto owner = ReadSessionOwner(reader);
    if (!request || !instance || !dbid || !type || !version || !owner || *request == 0 ||
        *instance == 0 || *dbid <= kInvalidDBID || *type == 0 || *version == 0 ||
        !IsValidSessionOwner(*owner) || reader.Remaining() != 0)
      return Error{ErrorCode::kInvalidArgument, "invalid session revocation request"};
    return RevokeAccountSession{*request, *instance, *dbid, *type, *version, *owner};
  }
};
static_assert(NetworkMessage<RevokeAccountSession>);

struct RevokeAccountSessionReply {
  uint64_t request_id{0};
  uint64_t requester_instance{0};
  DatabaseID account_dbid{kInvalidDBID};
  uint16_t account_type_id{0};
  uint64_t version{0};
  ErrorCode status{ErrorCode::kNone};
  std::string error;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::BaseApp::kRevokeAccountSessionReply),
                                   "baseapp::RevokeAccountSessionReply",
                                   MessageLengthStyle::kVariable,
                                   -1,
                                   MessageReliability::kReliable,
                                   MessageUrgency::kImmediate};
    return kDesc;
  }
  void Serialize(BinaryWriter& writer) const {
    writer.Write(request_id);
    writer.Write(requester_instance);
    writer.Write(account_dbid);
    writer.Write(account_type_id);
    writer.Write(version);
    writer.Write(static_cast<uint32_t>(status));
    writer.WriteString(error);
  }
  static auto Deserialize(BinaryReader& reader) -> Result<RevokeAccountSessionReply> {
    auto request = reader.Read<uint64_t>();
    auto instance = reader.Read<uint64_t>();
    auto dbid = reader.Read<DatabaseID>();
    auto type = reader.Read<uint16_t>();
    auto version = reader.Read<uint64_t>();
    auto status = reader.Read<uint32_t>();
    auto error = reader.ReadString();
    if (!request || !instance || !dbid || !type || !version || !status || !error || *request == 0 ||
        *instance == 0 || *dbid <= kInvalidDBID || *type == 0 || *version == 0 ||
        *status > static_cast<uint32_t>(ErrorCode::kCommitUnknown) || error->size() > 512 ||
        reader.Remaining() != 0)
      return Error{ErrorCode::kInvalidArgument, "invalid session revocation reply"};
    return RevokeAccountSessionReply{
        *request,         *instance, *dbid, *type, *version, static_cast<ErrorCode>(*status),
        std::move(*error)};
  }
};
static_assert(NetworkMessage<RevokeAccountSessionReply>);

}  // namespace atlas::baseapp

#endif  // ATLAS_SERVER_BASEAPP_SESSION_MESSAGES_H_
