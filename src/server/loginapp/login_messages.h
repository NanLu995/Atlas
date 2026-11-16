#ifndef ATLAS_SERVER_LOGINAPP_LOGIN_MESSAGES_H_
#define ATLAS_SERVER_LOGINAPP_LOGIN_MESSAGES_H_

#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "network/address.h"
#include "network/message.h"
#include "network/message_ids.h"
#include "server/entity_types.h"
#include "server/recovery_credential.h"

namespace atlas::login {

inline auto AreCredentialsValid(std::string_view username, std::string_view credential) -> bool {
  return !username.empty() && !credential.empty() &&
         username.find('\0') == std::string_view::npos &&
         credential.find('\0') == std::string_view::npos;
}

enum class LoginStatus : uint8_t {
  kSuccess = 0,
  kInvalidCredentials = 1,
  kAlreadyLoggedIn = 2,
  kServerFull = 3,
  kRateLimited = 4,
  kServerNotReady = 5,
  kInternalError = 6,
  kLoginInProgress = 7,
  kServerBusy = 8,
  kDefMismatch = 9,
};

struct LoginRequest {
  std::string username;
  std::string password_hash;
  // Client definition digest, checked by DBApp and BaseApp before account preparation.
  std::array<uint8_t, 32> entity_def_digest{};

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::Login::kLoginRequest),
                                   "login::LoginRequest",
                                   MessageLengthStyle::kVariable,
                                   -1,
                                   MessageReliability::kReliable,
                                   MessageUrgency::kImmediate};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.WriteString(username);
    w.WriteString(password_hash);
    w.WriteBytes(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(entity_def_digest.data()), entity_def_digest.size()));
  }

  static auto Deserialize(BinaryReader& r) -> Result<LoginRequest> {
    auto u = r.ReadString();
    auto p = r.ReadString();
    auto d = r.ReadBytes(32);
    if (!u || !p || !d) return Error{ErrorCode::kInvalidArgument, "LoginRequest: truncated"};
    if (!AreCredentialsValid(*u, *p))
      return Error{ErrorCode::kInvalidArgument, "LoginRequest: invalid credentials"};
    LoginRequest msg;
    msg.username = std::move(*u);
    msg.password_hash = std::move(*p);
    std::memcpy(msg.entity_def_digest.data(), d->data(), 32);
    return msg;
  }
};
static_assert(NetworkMessage<LoginRequest>);

struct ResumeRequest {
  RecoveryCredential credential;
  std::array<uint8_t, 32> entity_def_digest{};

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::Login::kResumeRequest),
                                   "login::ResumeRequest",
                                   MessageLengthStyle::kVariable,
                                   -1,
                                   MessageReliability::kReliable,
                                   MessageUrgency::kImmediate};
    return kDesc;
  }

  void Serialize(BinaryWriter& writer) const {
    WriteRecoveryCredential(writer, credential);
    writer.WriteBytes(std::as_bytes(std::span(entity_def_digest)));
  }

  static auto Deserialize(BinaryReader& reader) -> Result<ResumeRequest> {
    auto credential = ReadRecoveryCredential(reader);
    auto digest = reader.ReadBytes(32);
    if (!credential || !credential->IsPresent() || !digest || reader.Remaining() != 0)
      return Error{ErrorCode::kInvalidArgument, "invalid resume request"};
    ResumeRequest result;
    result.credential = *credential;
    std::memcpy(result.entity_def_digest.data(), digest->data(), 32);
    return result;
  }
};
static_assert(NetworkMessage<ResumeRequest>);

struct PrepareResume {
  uint64_t request_id{0};
  uint64_t requester_instance{0};
  DatabaseID account_dbid{kInvalidDBID};
  uint16_t account_type_id{0};
  std::array<std::byte, 32> proof{};
  SessionKey session_key;
  std::array<uint8_t, 32> entity_def_digest{};

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::Login::kPrepareResume),
                                   "login::PrepareResume",
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
    writer.WriteBytes(proof);
    writer.WriteBytes(std::as_bytes(std::span(session_key.bytes)));
    writer.WriteBytes(std::as_bytes(std::span(entity_def_digest)));
  }

  static auto Deserialize(BinaryReader& reader) -> Result<PrepareResume> {
    auto id = reader.Read<uint64_t>();
    auto instance = reader.Read<uint64_t>();
    auto dbid = reader.Read<DatabaseID>();
    auto type = reader.Read<uint16_t>();
    auto proof = reader.ReadBytes(32);
    auto key = reader.ReadBytes(sizeof(SessionKey));
    auto digest = reader.ReadBytes(32);
    if (!id || *id == 0 || !instance || *instance == 0 || !dbid || *dbid <= kInvalidDBID || !type ||
        *type == 0 || !proof || !key || !digest || reader.Remaining() != 0)
      return Error{ErrorCode::kInvalidArgument, "invalid prepare resume"};
    PrepareResume result;
    result.request_id = *id;
    result.requester_instance = *instance;
    result.account_dbid = *dbid;
    result.account_type_id = *type;
    std::memcpy(result.proof.data(), proof->data(), 32);
    std::memcpy(result.session_key.bytes, key->data(), sizeof(SessionKey));
    std::memcpy(result.entity_def_digest.data(), digest->data(), 32);
    if (result.session_key.IsZero() ||
        std::ranges::all_of(result.proof, [](auto value) { return value == std::byte{0}; }))
      return Error{ErrorCode::kInvalidArgument, "empty resume proof or session key"};
    return result;
  }
};
static_assert(NetworkMessage<PrepareResume>);

struct PrepareResumeResult {
  uint64_t request_id{0};
  uint64_t requester_instance{0};
  bool success{false};
  Address redirect;
  Address external_addr;
  EntityID entity_id{kInvalidEntityID};
  std::string error;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::Login::kPrepareResumeResult),
                                   "login::PrepareResumeResult",
                                   MessageLengthStyle::kVariable,
                                   -1,
                                   MessageReliability::kReliable,
                                   MessageUrgency::kImmediate};
    return kDesc;
  }

  void Serialize(BinaryWriter& writer) const {
    writer.Write(request_id);
    writer.Write(requester_instance);
    writer.Write(static_cast<uint8_t>(success));
    writer.Write(redirect.Ip());
    writer.Write(redirect.Port());
    writer.Write(external_addr.Ip());
    writer.Write(external_addr.Port());
    writer.Write(entity_id);
    writer.WriteString(error);
  }

  static auto Deserialize(BinaryReader& reader) -> Result<PrepareResumeResult> {
    auto id = reader.Read<uint64_t>();
    auto instance = reader.Read<uint64_t>();
    auto success = reader.Read<uint8_t>();
    auto ip = reader.Read<uint32_t>();
    auto port = reader.Read<uint16_t>();
    auto external_ip = reader.Read<uint32_t>();
    auto external_port = reader.Read<uint16_t>();
    auto entity = reader.Read<EntityID>();
    auto error = reader.ReadString();
    if (!id || *id == 0 || !instance || *instance == 0 || !success || *success > 1 || !ip ||
        !port || !external_ip || !external_port || !entity || !error || error->size() > 512 ||
        reader.Remaining() != 0 ||
        (*success != 0 && (*entity == kInvalidEntityID || *external_port == 0)))
      return Error{ErrorCode::kInvalidArgument, "invalid prepare resume result"};
    return PrepareResumeResult{*id,
                               *instance,
                               *success != 0,
                               Address(*ip, *port),
                               Address(*external_ip, *external_port),
                               *entity,
                               std::move(*error)};
  }
};
static_assert(NetworkMessage<PrepareResumeResult>);

struct LoginResult {
  LoginStatus status{LoginStatus::kInternalError};
  SessionKey session_key;
  Address baseapp_addr;
  std::string error_message;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{
        msg_id::Id(msg_id::Login::kLoginResult), "login::LoginResult",
        MessageLengthStyle::kVariable,           -1,
        MessageReliability::kReliable,           MessageUrgency::kImmediate};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write(static_cast<uint8_t>(status));
    if (status == LoginStatus::kSuccess) {
      w.WriteBytes(std::span<const std::byte>(reinterpret_cast<const std::byte*>(session_key.bytes),
                                              sizeof(session_key.bytes)));
      w.Write(baseapp_addr.Ip());
      w.Write(baseapp_addr.Port());
    } else {
      w.WriteString(error_message);
    }
  }

  static auto Deserialize(BinaryReader& r) -> Result<LoginResult> {
    auto st = r.Read<uint8_t>();
    if (!st) return Error{ErrorCode::kInvalidArgument, "LoginResult: truncated"};
    LoginResult msg;
    msg.status = static_cast<LoginStatus>(*st);
    if (msg.status == LoginStatus::kSuccess) {
      auto key_span = r.ReadBytes(sizeof(SessionKey));
      auto ip = r.Read<uint32_t>();
      auto port = r.Read<uint16_t>();
      if (!key_span || !ip || !port)
        return Error{ErrorCode::kInvalidArgument, "LoginResult: field truncated"};
      std::memcpy(msg.session_key.bytes, key_span->data(), sizeof(SessionKey));
      msg.baseapp_addr = Address(*ip, *port);
    } else {
      auto err = r.ReadString();
      if (!err) return Error{ErrorCode::kInvalidArgument, "LoginResult: error_message truncated"};
      msg.error_message = std::move(*err);
    }
    return msg;
  }
};
static_assert(NetworkMessage<LoginResult>);

struct AuthLogin {
  std::array<uint8_t, 32> entity_def_digest{};
  uint32_t request_id{0};
  std::string username;
  std::string password_hash;
  bool auto_create{false};

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{
        msg_id::Id(msg_id::Login::kAuthLogin), "login::AuthLogin",
        MessageLengthStyle::kVariable,         -1,
        MessageReliability::kReliable,         MessageUrgency::kImmediate};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write(request_id);
    w.WriteString(username);
    w.WriteString(password_hash);
    w.Write(static_cast<uint8_t>(auto_create ? 1 : 0));
    w.WriteBytes(std::as_bytes(std::span(entity_def_digest)));
  }

  static auto Deserialize(BinaryReader& r) -> Result<AuthLogin> {
    auto rid = r.Read<uint32_t>();
    auto u = r.ReadString();
    auto p = r.ReadString();
    auto ac = r.Read<uint8_t>();
    if (!rid || !u || !p || !ac) return Error{ErrorCode::kInvalidArgument, "AuthLogin: truncated"};
    if (!AreCredentialsValid(*u, *p))
      return Error{ErrorCode::kInvalidArgument, "AuthLogin: invalid credentials"};
    AuthLogin msg;
    msg.request_id = *rid;
    msg.username = std::move(*u);
    msg.password_hash = std::move(*p);
    msg.auto_create = (*ac != 0);
    auto digest = r.ReadBytes(msg.entity_def_digest.size());
    if (!digest) return digest.Error();
    std::memcpy(msg.entity_def_digest.data(), digest->data(), msg.entity_def_digest.size());
    return msg;
  }
};
static_assert(NetworkMessage<AuthLogin>);

struct AuthLoginResult {
  uint32_t request_id{0};
  bool success{false};
  LoginStatus status{LoginStatus::kInternalError};
  DatabaseID dbid{kInvalidDBID};
  uint16_t type_id{0};

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{
        msg_id::Id(msg_id::Login::kAuthLoginResult),
        "login::AuthLoginResult",
        MessageLengthStyle::kFixed,
        static_cast<int>(sizeof(uint32_t) + sizeof(uint8_t) + sizeof(uint8_t) + sizeof(int64_t) +
                         sizeof(uint16_t)),
        MessageReliability::kReliable,
        MessageUrgency::kImmediate};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write(request_id);
    w.Write(static_cast<uint8_t>(success ? 1 : 0));
    w.Write(static_cast<uint8_t>(status));
    w.Write(dbid);
    w.Write(type_id);
  }

  static auto Deserialize(BinaryReader& r) -> Result<AuthLoginResult> {
    auto rid = r.Read<uint32_t>();
    auto ok = r.Read<uint8_t>();
    auto st = r.Read<uint8_t>();
    auto db = r.Read<int64_t>();
    auto ti = r.Read<uint16_t>();
    if (!rid || !ok || !st || !db || !ti)
      return Error{ErrorCode::kInvalidArgument, "AuthLoginResult: truncated"};
    AuthLoginResult msg;
    msg.request_id = *rid;
    msg.success = (*ok != 0);
    msg.status = static_cast<LoginStatus>(*st);
    msg.dbid = *db;
    msg.type_id = *ti;
    return msg;
  }
};
static_assert(NetworkMessage<AuthLoginResult>);

struct AllocateBaseApp {
  uint32_t request_id{0};
  uint16_t type_id{0};
  DatabaseID dbid{kInvalidDBID};

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{
        msg_id::Id(msg_id::Login::kAllocateBaseApp),
        "login::AllocateBaseApp",
        MessageLengthStyle::kFixed,
        static_cast<int>(sizeof(uint32_t) + sizeof(uint16_t) + sizeof(int64_t)),
        MessageReliability::kReliable,
        MessageUrgency::kImmediate};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write(request_id);
    w.Write(type_id);
    w.Write(dbid);
  }

  static auto Deserialize(BinaryReader& r) -> Result<AllocateBaseApp> {
    auto rid = r.Read<uint32_t>();
    auto ti = r.Read<uint16_t>();
    auto db = r.Read<int64_t>();
    if (!rid || !ti || !db) return Error{ErrorCode::kInvalidArgument, "AllocateBaseApp: truncated"};
    AllocateBaseApp msg;
    msg.request_id = *rid;
    msg.type_id = *ti;
    msg.dbid = *db;
    return msg;
  }
};
static_assert(NetworkMessage<AllocateBaseApp>);

struct AllocateBaseAppResult {
  uint32_t request_id{0};
  bool success{false};
  Address internal_addr;
  Address external_addr;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::Login::kAllocateBaseAppResult),
                                   "login::AllocateBaseAppResult",
                                   MessageLengthStyle::kFixed,
                                   static_cast<int>(sizeof(uint32_t) + sizeof(uint8_t) +
                                                    (sizeof(uint32_t) + sizeof(uint16_t)) * 2),
                                   MessageReliability::kReliable,
                                   MessageUrgency::kImmediate};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write(request_id);
    w.Write(static_cast<uint8_t>(success ? 1 : 0));
    w.Write(internal_addr.Ip());
    w.Write(internal_addr.Port());
    w.Write(external_addr.Ip());
    w.Write(external_addr.Port());
  }

  static auto Deserialize(BinaryReader& r) -> Result<AllocateBaseAppResult> {
    auto rid = r.Read<uint32_t>();
    auto ok = r.Read<uint8_t>();
    auto iip = r.Read<uint32_t>();
    auto iport = r.Read<uint16_t>();
    auto eip = r.Read<uint32_t>();
    auto eport = r.Read<uint16_t>();
    if (!rid || !ok || !iip || !iport || !eip || !eport)
      return Error{ErrorCode::kInvalidArgument, "AllocateBaseAppResult: truncated"};
    AllocateBaseAppResult msg;
    msg.request_id = *rid;
    msg.success = (*ok != 0);
    msg.internal_addr = Address(*iip, *iport);
    msg.external_addr = Address(*eip, *eport);
    return msg;
  }
};
static_assert(NetworkMessage<AllocateBaseAppResult>);

struct PrepareLogin {
  static constexpr uint32_t kMaxBlobSize = 1024 * 1024;

  uint32_t request_id{0};
  uint16_t type_id{0};
  DatabaseID dbid{kInvalidDBID};
  SessionKey session_key;
  Address client_addr;
  std::vector<std::byte> entity_blob;
  bool blob_prefetched{false};
  // Stamped from LoginRequest; BaseApp matches it against its own digest
  // so a mismatched .def build is rejected before any RPC dispatch.
  std::array<uint8_t, 32> entity_def_digest{};
  // Carried through so BaseApp can key its account_entity_index_ on the
  // user-supplied identity and serialize same-name re-logins.
  std::string username;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::Login::kPrepareLogin),
                                   "login::PrepareLogin",
                                   MessageLengthStyle::kVariable,
                                   -1,
                                   MessageReliability::kReliable,
                                   MessageUrgency::kImmediate};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write(request_id);
    w.Write(type_id);
    w.Write(dbid);
    w.WriteBytes(std::span<const std::byte>(reinterpret_cast<const std::byte*>(session_key.bytes),
                                            sizeof(session_key.bytes)));
    w.Write(client_addr.Ip());
    w.Write(client_addr.Port());
    w.Write(static_cast<uint8_t>(blob_prefetched ? 1 : 0));
    w.Write(static_cast<uint32_t>(entity_blob.size()));
    if (!entity_blob.empty()) w.WriteBytes(entity_blob);
    w.WriteBytes(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(entity_def_digest.data()), entity_def_digest.size()));
    w.WriteString(username);
  }

  static auto Deserialize(BinaryReader& r) -> Result<PrepareLogin> {
    auto rid = r.Read<uint32_t>();
    auto ti = r.Read<uint16_t>();
    auto db = r.Read<int64_t>();
    if (!rid || !ti || !db) return Error{ErrorCode::kInvalidArgument, "PrepareLogin: truncated"};
    auto key_span = r.ReadBytes(sizeof(SessionKey));
    auto ip = r.Read<uint32_t>();
    auto port = r.Read<uint16_t>();
    auto pf = r.Read<uint8_t>();
    auto blob_sz = r.Read<uint32_t>();
    if (!key_span || !ip || !port || !pf || !blob_sz)
      return Error{ErrorCode::kInvalidArgument, "PrepareLogin: field truncated"};
    PrepareLogin msg;
    msg.request_id = *rid;
    msg.type_id = *ti;
    msg.dbid = *db;
    std::memcpy(msg.session_key.bytes, key_span->data(), sizeof(SessionKey));
    msg.client_addr = Address(*ip, *port);
    msg.blob_prefetched = (*pf != 0);
    if (*blob_sz > kMaxBlobSize)
      return Error{ErrorCode::kInvalidArgument, "PrepareLogin: blob too large"};
    if (*blob_sz > 0) {
      auto blob_span = r.ReadBytes(*blob_sz);
      if (!blob_span) return Error{ErrorCode::kInvalidArgument, "PrepareLogin: blob truncated"};
      msg.entity_blob.assign(blob_span->begin(), blob_span->end());
    }
    auto digest_span = r.ReadBytes(32);
    if (!digest_span) return Error{ErrorCode::kInvalidArgument, "PrepareLogin: digest truncated"};
    std::memcpy(msg.entity_def_digest.data(), digest_span->data(), 32);
    auto name = r.ReadString();
    if (!name) return Error{ErrorCode::kInvalidArgument, "PrepareLogin: username truncated"};
    msg.username = std::move(*name);
    return msg;
  }
};
static_assert(NetworkMessage<PrepareLogin>);

struct PrepareLoginResult {
  uint32_t request_id{0};
  bool success{false};
  EntityID entity_id{kInvalidEntityID};
  std::string error;

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::Login::kPrepareLoginResult),
                                   "login::PrepareLoginResult",
                                   MessageLengthStyle::kVariable,
                                   -1,
                                   MessageReliability::kReliable,
                                   MessageUrgency::kImmediate};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write(request_id);
    w.Write(static_cast<uint8_t>(success ? 1 : 0));
    w.Write(entity_id);
    w.WriteString(error);
  }

  static auto Deserialize(BinaryReader& r) -> Result<PrepareLoginResult> {
    auto rid = r.Read<uint32_t>();
    auto ok = r.Read<uint8_t>();
    auto eid = r.Read<uint32_t>();
    auto err = r.ReadString();
    if (!rid || !ok || !eid || !err)
      return Error{ErrorCode::kInvalidArgument, "PrepareLoginResult: truncated"};
    PrepareLoginResult msg;
    msg.request_id = *rid;
    msg.success = (*ok != 0);
    msg.entity_id = *eid;
    msg.error = std::move(*err);
    return msg;
  }
};
static_assert(NetworkMessage<PrepareLoginResult>);

// Sent when the client disconnects before the login handoff completes so the
// target BaseApp can roll back any pending prepare / checkout work.
struct CancelPrepareLogin {
  uint32_t request_id{0};
  DatabaseID dbid{kInvalidDBID};

  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc kDesc{msg_id::Id(msg_id::Login::kCancelPrepareLogin),
                                   "login::CancelPrepareLogin",
                                   MessageLengthStyle::kFixed,
                                   static_cast<int>(sizeof(uint32_t) + sizeof(int64_t)),
                                   MessageReliability::kReliable,
                                   MessageUrgency::kImmediate};
    return kDesc;
  }

  void Serialize(BinaryWriter& w) const {
    w.Write(request_id);
    w.Write(dbid);
  }

  static auto Deserialize(BinaryReader& r) -> Result<CancelPrepareLogin> {
    auto rid = r.Read<uint32_t>();
    auto db = r.Read<int64_t>();
    if (!rid || !db) return Error{ErrorCode::kInvalidArgument, "CancelPrepareLogin: truncated"};
    CancelPrepareLogin msg;
    msg.request_id = *rid;
    msg.dbid = *db;
    return msg;
  }
};
static_assert(NetworkMessage<CancelPrepareLogin>);

}  // namespace atlas::login

#endif  // ATLAS_SERVER_LOGINAPP_LOGIN_MESSAGES_H_
