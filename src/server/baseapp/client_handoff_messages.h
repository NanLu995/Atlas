#ifndef ATLAS_SERVER_BASEAPP_CLIENT_HANDOFF_MESSAGES_H_
#define ATLAS_SERVER_BASEAPP_CLIENT_HANDOFF_MESSAGES_H_

#include "db/account_session.h"
#include "network/message.h"
#include "network/message_ids.h"

namespace atlas::baseapp {

struct PrepareClientHandoff {
  uint64_t request_id{0};
  uint64_t requester_instance{0};
  DatabaseID account_dbid{kInvalidDBID};
  uint16_t account_type_id{0};
  AccountSession expected;
  std::array<std::byte, 32> entity_def_digest{};
  std::string username;
  bool wait_for_client{false};

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::BaseApp::kPrepareClientHandoff),
                                   "baseapp::PrepareClientHandoff",
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
    WriteAccountSession(writer, expected);
    writer.WriteBytes(entity_def_digest);
    writer.WriteString(username);
    writer.Write(static_cast<uint8_t>(wait_for_client));
  }
  static auto Deserialize(BinaryReader& reader) -> Result<PrepareClientHandoff> {
    auto request = reader.Read<uint64_t>();
    auto instance = reader.Read<uint64_t>();
    auto dbid = reader.Read<DatabaseID>();
    auto type = reader.Read<uint16_t>();
    auto expected = ReadAccountSession(reader);
    auto digest = reader.ReadBytes(32);
    auto username = reader.ReadString();
    auto wait = reader.Read<uint8_t>();
    if (!request || !instance || !dbid || !type || !expected || !digest || !username ||
        *request == 0 || *instance == 0 || *dbid <= kInvalidDBID || *type == 0 ||
        username->empty() || username->size() > 128 ||
        expected->phase != AccountSessionPhase::kPreparing ||
        expected->owner.process_instance != *instance || !wait || *wait > 1 ||
        reader.Remaining() != 0)
      return Error{ErrorCode::kInvalidArgument, "invalid client handoff request"};
    PrepareClientHandoff result;
    result.request_id = *request;
    result.requester_instance = *instance;
    result.account_dbid = *dbid;
    result.account_type_id = *type;
    result.expected = *expected;
    std::ranges::copy(*digest, result.entity_def_digest.begin());
    result.username = std::move(*username);
    result.wait_for_client = *wait != 0;
    return result;
  }
};
static_assert(NetworkMessage<PrepareClientHandoff>);

struct PrepareClientHandoffReply {
  uint64_t request_id{0};
  uint64_t requester_instance{0};
  DatabaseID account_dbid{kInvalidDBID};
  uint16_t account_type_id{0};
  ErrorCode status{ErrorCode::kNone};
  std::string error;
  AccountSession session;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::BaseApp::kPrepareClientHandoffReply),
                                   "baseapp::PrepareClientHandoffReply",
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
    writer.Write(static_cast<uint32_t>(status));
    writer.WriteString(error);
    WriteAccountSession(writer, session);
  }
  static auto Deserialize(BinaryReader& reader) -> Result<PrepareClientHandoffReply> {
    auto request = reader.Read<uint64_t>();
    auto instance = reader.Read<uint64_t>();
    auto dbid = reader.Read<DatabaseID>();
    auto type = reader.Read<uint16_t>();
    auto status = reader.Read<uint32_t>();
    auto error = reader.ReadString();
    auto session = ReadAccountSession(reader);
    if (!request || !instance || !dbid || !type || !status || !error || !session || *request == 0 ||
        *instance == 0 || *dbid <= kInvalidDBID || *type == 0 ||
        *status > static_cast<uint32_t>(ErrorCode::kCommitUnknown) || error->size() > 512 ||
        reader.Remaining() != 0)
      return Error{ErrorCode::kInvalidArgument, "invalid client handoff reply"};
    return PrepareClientHandoffReply{
        *request,          *instance, *dbid, *type, static_cast<ErrorCode>(*status),
        std::move(*error), *session};
  }
};
static_assert(NetworkMessage<PrepareClientHandoffReply>);

struct ClientHandoff {
  AccountSessionOwner target;
  SessionKey credential;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::BaseApp::kClientHandoff),
                                   "baseapp::ClientHandoff",
                                   MessageLengthStyle::kVariable,
                                   -1,
                                   MessageReliability::kReliable,
                                   MessageUrgency::kImmediate};
    return kDesc;
  }
  void Serialize(BinaryWriter& writer) const {
    WriteSessionOwner(writer, target);
    writer.WriteBytes(std::as_bytes(std::span{credential.bytes}));
  }
  static auto Deserialize(BinaryReader& reader) -> Result<ClientHandoff> {
    auto target = ReadSessionOwner(reader);
    auto key = reader.ReadBytes(32);
    if (!target || !IsValidSessionOwner(*target) || !key || reader.Remaining() != 0)
      return Error{ErrorCode::kInvalidArgument, "invalid client handoff destination"};
    ClientHandoff result;
    result.target = *target;
    std::ranges::transform(*key, result.credential.bytes,
                           [](std::byte value) { return std::to_integer<uint8_t>(value); });
    if (result.credential.IsZero())
      return Error{ErrorCode::kInvalidArgument, "empty client handoff credential"};
    return result;
  }
};
static_assert(NetworkMessage<ClientHandoff>);

}  // namespace atlas::baseapp

#endif  // ATLAS_SERVER_BASEAPP_CLIENT_HANDOFF_MESSAGES_H_
