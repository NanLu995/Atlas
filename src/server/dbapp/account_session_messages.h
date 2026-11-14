#ifndef ATLAS_SERVER_DBAPP_ACCOUNT_SESSION_MESSAGES_H_
#define ATLAS_SERVER_DBAPP_ACCOUNT_SESSION_MESSAGES_H_

#include "db/account_session.h"
#include "network/message.h"
#include "network/message_ids.h"

namespace atlas::dbapp {

struct AccountSessionRequest {
  uint64_t request_id{0};
  uint32_t shard_table_version{0};
  uint64_t process_instance{0};
  DatabaseID account_dbid{kInvalidDBID};
  uint16_t account_type_id{0};
  std::optional<AccountSessionOperation> operation;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::DBApp::kAccountSessionRequest),
                                   "dbapp::AccountSessionRequest",
                                   MessageLengthStyle::kVariable,
                                   -1,
                                   MessageReliability::kReliable,
                                   MessageUrgency::kImmediate};
    return kDesc;
  }

  void Serialize(BinaryWriter& writer) const {
    writer.Write(request_id);
    writer.Write(shard_table_version);
    writer.Write(process_instance);
    writer.Write(account_dbid);
    writer.Write(account_type_id);
    writer.Write(static_cast<uint8_t>(operation.has_value()));
    if (operation) WriteSessionOperation(writer, *operation);
  }

  static auto Deserialize(BinaryReader& reader) -> Result<AccountSessionRequest> {
    auto request = reader.Read<uint64_t>();
    auto shard = reader.Read<uint32_t>();
    auto instance = reader.Read<uint64_t>();
    auto dbid = reader.Read<DatabaseID>();
    auto type = reader.Read<uint16_t>();
    auto update = reader.Read<uint8_t>();
    if (!request || !shard || !instance || !dbid || !type || !update || *update > 1)
      return Error{ErrorCode::kInvalidArgument, "truncated account session request"};
    AccountSessionRequest result;
    result.request_id = *request;
    result.shard_table_version = *shard;
    result.process_instance = *instance;
    result.account_dbid = *dbid;
    result.account_type_id = *type;
    if (*update != 0) {
      auto op = ReadSessionOperation(reader);
      if (!op) return op.Error();
      result.operation = *op;
    }
    if (reader.Remaining() != 0)
      return Error{ErrorCode::kInvalidArgument, "account session request trailing data"};
    return result;
  }
};
static_assert(NetworkMessage<AccountSessionRequest>);

struct AccountSessionReply {
  uint64_t request_id{0};
  uint64_t requester_instance{0};
  DatabaseID account_dbid{kInvalidDBID};
  uint16_t account_type_id{0};
  std::array<std::byte, 16> operation_id{};
  ErrorCode status{ErrorCode::kNone};
  std::string error;
  AccountSession session;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::DBApp::kAccountSessionReply),
                                   "dbapp::AccountSessionReply",
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
    writer.WriteBytes(operation_id);
    writer.Write(static_cast<uint32_t>(status));
    writer.WriteString(error);
    WriteAccountSession(writer, session);
  }

  static auto Deserialize(BinaryReader& reader) -> Result<AccountSessionReply> {
    auto request = reader.Read<uint64_t>();
    auto instance = reader.Read<uint64_t>();
    auto dbid = reader.Read<DatabaseID>();
    auto type = reader.Read<uint16_t>();
    auto op = reader.ReadBytes(16);
    auto status = reader.Read<uint32_t>();
    auto error = reader.ReadString();
    auto session = ReadAccountSession(reader);
    if (!request || !instance || !dbid || !type || !op || !status || !error || !session ||
        error->size() > 512 || *status > static_cast<uint32_t>(ErrorCode::kCommitUnknown) ||
        reader.Remaining() != 0)
      return Error{ErrorCode::kInvalidArgument, "invalid account session reply"};
    AccountSessionReply result;
    result.request_id = *request;
    result.requester_instance = *instance;
    result.account_dbid = *dbid;
    result.account_type_id = *type;
    std::ranges::copy(*op, result.operation_id.begin());
    result.status = static_cast<ErrorCode>(*status);
    result.error = std::move(*error);
    result.session = *session;
    return result;
  }
};
static_assert(NetworkMessage<AccountSessionReply>);

}  // namespace atlas::dbapp

#endif  // ATLAS_SERVER_DBAPP_ACCOUNT_SESSION_MESSAGES_H_
