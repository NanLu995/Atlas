#include <algorithm>
#include <format>

#include "baseapp.h"
#include "coro/rpc_call.h"
#include "entitydef/entity_def_registry.h"
#include "foundation/log.h"
#include "loginapp/login_messages.h"
#include "platform/secure_random.h"

namespace atlas {

void BaseApp::OnBaseAppBirth(const machined::BirthNotification& notification) {
  if (notification.process_type != ProcessType::kBaseApp || notification.process_instance == 0 ||
      notification.internal_addr.Ip() == 0 || notification.internal_addr.Port() == 0)
    return;
  baseapp_peers_.insert_or_assign(notification.internal_addr, notification);
}

void BaseApp::OnBaseAppDeath(const machined::DeathNotification& notification) {
  const auto peer = baseapp_peers_.find(notification.internal_addr);
  if (peer != baseapp_peers_.end() &&
      peer->second.process_instance == notification.process_instance)
    baseapp_peers_.erase(peer);
}

void BaseApp::OnRevokeAccountSessionReply(Channel* channel,
                                          const baseapp::RevokeAccountSessionReply& reply) {
  if (reply.requester_instance != Config().process_instance) return;
  BinaryWriter payload;
  reply.Serialize(payload);
  (void)rpc_registry_.TryDispatch(reply.Descriptor().id, payload.Data(), channel);
}

auto BaseApp::OnRevokeAccountSession(IntrusivePtr<Channel> channel,
                                     baseapp::RevokeAccountSession request) -> FireAndForget {
  auto respond = [&](Result<void> result) {
    baseapp::RevokeAccountSessionReply reply;
    reply.request_id = request.request_id;
    reply.requester_instance = request.requester_instance;
    reply.account_dbid = request.account_dbid;
    reply.account_type_id = request.account_type_id;
    reply.version = request.version;
    if (!result) {
      reply.status = result.Error().Code();
      reply.error = result.Error().Message();
      if (reply.error.size() > 512) reply.error.resize(512);
    }
    if (!channel->IsCondemned()) (void)channel->SendMessage(reply);
  };
  const auto peer = baseapp_peers_.find(channel->RemoteAddress());
  const auto self = baseapp_peers_.find(request.owner.base_addr);
  if (request.request_id == 0 || request.requester_instance == 0 ||
      request.account_dbid <= kInvalidDBID || request.account_type_id == 0 ||
      request.version == 0 || !IsValidSessionOwner(request.owner)) {
    respond(Error{ErrorCode::kInvalidArgument, "invalid session revocation request"});
    co_return;
  }
  if (peer == baseapp_peers_.end() || peer->second.process_instance != request.requester_instance ||
      request.owner.process_instance != Config().process_instance || self == baseapp_peers_.end() ||
      self->second.process_instance != Config().process_instance) {
    respond(Error{ErrorCode::kPermissionDenied, "unregistered session revocation process"});
    co_return;
  }
  if (session_revocations_ >= 1024 || rpc_registry_.IsStopping()) {
    respond(Error{ErrorCode::kRateLimited, "session revocation unavailable"});
    co_return;
  }
  ++session_revocations_;
  auto record = co_await QueryAccountSession(request.account_dbid, request.account_type_id, {});
  Result<void> result;
  if (!record) {
    result = record.Error();
  } else if (record->phase == AccountSessionPhase::kOffline) {
    result = {};
  } else if (record->version != request.version || record->owner != request.owner) {
    result = Error{ErrorCode::kPermissionDenied, "session revocation owner or version changed"};
  } else if (const auto local = client_sessions_.find(request.owner.entity_id);
             local != client_sessions_.end()) {
    if (local->second->account_dbid != request.account_dbid ||
        local->second->account_type_id != request.account_type_id ||
        local->second->owner != request.owner) {
      result = Error{ErrorCode::kPermissionDenied, "session revocation local owner mismatch"};
    } else {
      result = co_await ReleaseClientSession(request.owner.entity_id, request.version);
    }
  } else {
    auto* proxy = entity_mgr_.FindProxy(request.owner.entity_id);
    if (proxy && proxy->HasClient()) {
      result = Error{ErrorCode::kPermissionDenied, "bound client session cannot be confirmed"};
    } else {
      AccountSessionOperation release;
      release.account_dbid = request.account_dbid;
      release.account_type_id = request.account_type_id;
      release.expected_version = request.version;
      release.actor = request.owner;
      release.action = AccountSessionAction::kRelease;
      auto random = FillSecureRandom(release.id);
      if (!random) {
        result = random.Error();
      } else {
        auto released =
            co_await QueryAccountSession(request.account_dbid, request.account_type_id, release);
        if (!released) result = released.Error();
      }
    }
  }
  --session_revocations_;
  respond(std::move(result));
}

auto BaseApp::RevokeSessionOwner(DatabaseID dbid, uint16_t type_id, AccountSession record,
                                 CancellationToken token) -> Task<Result<void>> {
  const auto target = baseapp_peers_.find(record.owner.base_addr);
  if (target == baseapp_peers_.end() ||
      target->second.process_instance != record.owner.process_instance)
    co_return Error{ErrorCode::kReceiverGone, "account session owner is not registered"};
  if (record.owner.process_instance == Config().process_instance) {
    const auto local = client_sessions_.find(record.owner.entity_id);
    if (local == client_sessions_.end() || local->second->account_dbid != dbid ||
        local->second->account_type_id != type_id || local->second->owner != record.owner)
      co_return Error{ErrorCode::kPermissionDenied, "local account session cannot be confirmed"};
    co_return co_await ReleaseClientSession(record.owner.entity_id, record.version);
  }
  auto* route = ResolveInternalChannel(record.owner.base_addr);
  if (route == nullptr || route->IsCondemned())
    co_return Error{ErrorCode::kReceiverGone, "account session owner channel unavailable"};
  IntrusivePtr<Channel> channel{route};
  baseapp::RevokeAccountSession request;
  request.request_id = rpc_registry_.AllocateRequestId();
  if (request.request_id == 0)
    co_return Error{ErrorCode::kOutOfRange, "session revocation IDs exhausted"};
  request.requester_instance = Config().process_instance;
  request.account_dbid = dbid;
  request.account_type_id = type_id;
  request.version = record.version;
  request.owner = record.owner;
  auto reply = co_await RpcCall<baseapp::RevokeAccountSessionReply>(
      rpc_registry_, *channel, request, std::chrono::seconds(5), std::move(token));
  if (!reply) co_return reply.Error();
  if (reply->requester_instance != request.requester_instance || reply->account_dbid != dbid ||
      reply->account_type_id != type_id || reply->version != record.version)
    co_return Error{ErrorCode::kInvalidArgument, "session revocation reply mismatch"};
  if (reply->status != ErrorCode::kNone) co_return Error{reply->status, reply->error};
  co_return Result<void>{};
}

auto BaseApp::CheckSessionBeforeLogin(PendingLogin pending, uint64_t ticket) -> FireAndForget {
  const auto found = login_session_checks_.find(ticket);
  if (found == login_session_checks_.end()) co_return;
  const auto token = found->second.cancel.Token();
  auto record = co_await QueryAccountSession(pending.dbid, pending.type_id, {}, token);
  Result<void> result;
  if (!record) {
    result = record.Error();
  } else if (record->phase != AccountSessionPhase::kOffline) {
    result = co_await RevokeSessionOwner(pending.dbid, pending.type_id, *record, token);
  }
  login_session_checks_.erase(ticket);
  if (token.IsCancelled()) co_return;
  if (!result) {
    FailPendingPrepareLogin(
        pending, std::format("account_session_{}", ErrorCodeName(result.Error().Code())));
    co_return;
  }
  SubmitPrepareLogin(std::move(pending));
}

void BaseApp::OnPrepareLogin(Channel& ch, const login::PrepareLogin& msg) {
  if (IsShuttingDown()) {
    login::PrepareLoginResult reply;
    reply.request_id = msg.request_id;
    reply.error = "server_stopping";
    (void)ch.SendMessage(reply);
    return;
  }
  ATLAS_LOG_DEBUG("BaseApp: prepare login request_id={} dbid={} type_id={} blob={}B from {}:{}",
                  msg.request_id, msg.dbid, msg.type_id, msg.entity_blob.size(),
                  ch.RemoteAddress().Ip(), ch.RemoteAddress().Port());

  const auto server_digest = EntityDefRegistry::Instance().Digest();
  if (!std::equal(msg.entity_def_digest.begin(), msg.entity_def_digest.end(), server_digest.begin(),
                  server_digest.end())) {
    ATLAS_LOG_WARNING(
        "BaseApp: PrepareLogin rejected request_id={} dbid={}: entity-def digest mismatch",
        msg.request_id, msg.dbid);
    login::PrepareLoginResult result;
    result.request_id = msg.request_id;
    result.success = false;
    result.error = "def_mismatch";
    if (auto r = ch.SendMessage(result); !r) {
      ATLAS_LOG_WARNING("BaseApp: PrepareLoginResult(def_mismatch) send failed: {}",
                        r.Error().Message());
    }
    return;
  }

  PendingLogin pending;
  pending.entity_def_digest = msg.entity_def_digest;
  pending.login_request_id = msg.request_id;
  pending.loginapp_addr = ch.RemoteAddress();
  pending.type_id = msg.type_id;
  pending.dbid = msg.dbid;
  pending.session_key = msg.session_key;
  pending.created_at = Clock::now();
  pending.blob_prefetched = msg.blob_prefetched;
  pending.entity_blob = msg.entity_blob;
  pending.username = msg.username;
  if (login_session_checks_.size() >= 1024 || rpc_registry_.IsStopping()) {
    FailPendingPrepareLogin(pending, "account_session_check_unavailable");
    return;
  }
  const auto ticket = rpc_registry_.AllocateRequestId();
  if (ticket == 0) {
    FailPendingPrepareLogin(pending, "account_session_request_ids_exhausted");
    return;
  }
  login_session_checks_.emplace(
      ticket, LoginSessionCheck{pending.loginapp_addr, pending.login_request_id, pending.dbid, {}});
  CheckSessionBeforeLogin(std::move(pending), ticket);
  (void)msg.client_addr;
}

void BaseApp::OnCancelPrepareLogin(Channel& channel, const login::CancelPrepareLogin& msg) {
  std::vector<CancellationSource> cancellations;
  for (const auto& [_, pending] : login_session_checks_) {
    if (pending.loginapp_addr == channel.RemoteAddress() &&
        pending.login_request_id == msg.request_id && pending.dbid == msg.dbid)
      cancellations.push_back(pending.cancel);
  }
  for (auto& cancel : cancellations) cancel.RequestCancellation();
  CancelPrepareLogin(msg.request_id, msg.dbid);
}

}  // namespace atlas
