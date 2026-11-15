#include <cstring>

#include "baseapp.h"
#include "platform/sha256.h"

namespace atlas {
namespace {

auto CredentialIndex(const Sha256Digest& digest) -> SessionKey {
  SessionKey key;
  std::memcpy(key.bytes, digest.data(), digest.size());
  return key;
}

auto MatchesHandoff(const AccountSession& record, const AccountSession& prepared) -> bool {
  return record.phase >= AccountSessionPhase::kPreparing && record.owner == prepared.owner &&
         record.target == prepared.target && record.handoff_id == prepared.handoff_id &&
         record.credential_hash == prepared.credential_hash && record.version >= prepared.version;
}

void ReplyToHandoff(Channel& channel, const baseapp::PrepareClientHandoff& request,
                    Result<AccountSession> result) {
  baseapp::PrepareClientHandoffReply reply;
  reply.request_id = request.request_id;
  reply.requester_instance = request.requester_instance;
  reply.account_dbid = request.account_dbid;
  reply.account_type_id = request.account_type_id;
  if (result) {
    reply.session = *result;
  } else {
    reply.status = result.Error().Code();
    reply.error = result.Error().Message().substr(0, 512);
  }
  if (!channel.IsCondemned()) (void)channel.SendMessage(reply);
}

}  // namespace

void BaseApp::RegisterClientHandoffHandlers() {
  auto& table = Network().InterfaceTable();
  (void)table.RegisterTypedHandler<baseapp::PrepareClientHandoff>(
      [this](const Address&, Channel* channel, const baseapp::PrepareClientHandoff& request) {
        if (channel) OnPrepareClientHandoff(IntrusivePtr<Channel>{channel}, request);
      });
  (void)table.RegisterTypedHandler<baseapp::PrepareClientHandoffReply>(
      [this](const Address&, Channel* channel, const baseapp::PrepareClientHandoffReply& reply) {
        if (reply.requester_instance != Config().process_instance) return;
        BinaryWriter payload;
        reply.Serialize(payload);
        (void)rpc_registry_.TryDispatch(reply.Descriptor().id, payload.Data(), channel);
      });
}

auto BaseApp::IsCurrentIncomingClientHandoff(const IncomingClientHandoff& incoming) const -> bool {
  const auto entity_id = incoming.session->owner.entity_id;
  const auto found = incoming_client_handoffs_.find(entity_id);
  const auto session = client_sessions_.find(entity_id);
  return found != incoming_client_handoffs_.end() && found->second.get() == &incoming &&
         session != client_sessions_.end() && session->second == incoming.session;
}

auto BaseApp::OnPrepareClientHandoff(IntrusivePtr<Channel> channel,
                                     baseapp::PrepareClientHandoff request) -> FireAndForget {
  auto respond = [&](Result<AccountSession> result) {
    ReplyToHandoff(*channel, request, std::move(result));
  };
  const auto& prepared = request.expected;
  const auto peer = baseapp_peers_.find(channel->RemoteAddress());
  const auto self = baseapp_peers_.find(prepared.target.base_addr);
  const auto digest = EntityDefRegistry::Instance().Digest();
  if (!ValidateAccountSession(prepared) || prepared.phase != AccountSessionPhase::kPreparing ||
      request.request_id == 0 || request.account_dbid <= kInvalidDBID ||
      request.account_type_id == 0 || request.username.empty() || request.username.size() > 128 ||
      !std::ranges::equal(std::as_bytes(digest), request.entity_def_digest)) {
    respond(Error{ErrorCode::kInvalidArgument, "invalid client handoff contract"});
    co_return;
  }
  if (peer == baseapp_peers_.end() || peer->second.process_instance != request.requester_instance ||
      prepared.owner.process_instance != request.requester_instance ||
      prepared.owner.base_addr != channel->RemoteAddress() || self == baseapp_peers_.end() ||
      self->second.process_instance != Config().process_instance ||
      prepared.target.process_instance != Config().process_instance) {
    respond(Error{ErrorCode::kPermissionDenied, "unregistered client handoff process"});
    co_return;
  }
  if (IsShuttingDown() || rpc_registry_.IsStopping()) {
    respond(Error{ErrorCode::kReceiverGone, "BaseApp is stopping"});
    co_return;
  }
  if (request.wait_for_client) {
    OnAwaitClientHandoff(std::move(channel), request);
    co_return;
  }
  const auto entity_id = prepared.target.entity_id;
  auto target_available = [&] {
    auto* proxy = entity_mgr_.FindProxy(entity_id);
    return proxy && !proxy->IsPendingDestroy() && !proxy->HasClient() &&
           proxy->GetSessionKey().IsZero();
  };
  std::shared_ptr<IncomingClientHandoff> incoming;
  if (const auto found = incoming_client_handoffs_.find(entity_id);
      found != incoming_client_handoffs_.end()) {
    incoming = found->second;
    const auto& previous = incoming->request;
    if (previous.expected != prepared || previous.account_dbid != request.account_dbid ||
        previous.account_type_id != request.account_type_id ||
        previous.username != request.username) {
      respond(Error{ErrorCode::kAlreadyExists, "client handoff target reserved"});
      co_return;
    }
    if (incoming->session->busy || incoming->authenticating || incoming->expiring ||
        incoming->session->revoking || incoming->session->wait_cancel.Token().IsCancelled() ||
        !IsCurrentIncomingClientHandoff(*incoming) || !target_available()) {
      respond(Error{ErrorCode::kWouldBlock, "client handoff confirmation pending"});
      co_return;
    }
  } else {
    auto* proxy = entity_mgr_.FindProxy(entity_id);
    const auto index = CredentialIndex(prepared.credential_hash);
    if (incoming_client_handoffs_.size() >= 1024) {
      respond(Error{ErrorCode::kRateLimited, "client handoff queue full"});
      co_return;
    }
    if (!proxy || proxy->IsPendingDestroy() || proxy->HasClient() ||
        !proxy->GetSessionKey().IsZero() || client_sessions_.contains(entity_id) ||
        client_transfer_targets_.contains(entity_id) ||
        prepared_login_requests_by_entity_.contains(entity_id) ||
        incoming_handoff_credentials_.contains(index)) {
      respond(Error{ErrorCode::kAlreadyExists, "client handoff target unavailable"});
      co_return;
    }
    incoming = std::make_shared<IncomingClientHandoff>();
    incoming->request = request;
    incoming->credential_hash = index;
    auto session = std::make_shared<ClientSession>();
    session->account_dbid = request.account_dbid;
    session->account_type_id = request.account_type_id;
    session->owner = prepared.target;
    incoming->session = session;
    incoming_client_handoffs_.emplace(entity_id, incoming);
    incoming_handoff_credentials_.emplace(index, entity_id);
    client_sessions_.emplace(entity_id, std::move(session));
    client_transfer_targets_.insert(entity_id);
  }
  auto session = incoming->session;
  session->busy = true;
  auto record = co_await QueryAccountSession(request.account_dbid, request.account_type_id, {},
                                             session->wait_cancel.Token());
  session->busy = false;
  if (!IsCurrentIncomingClientHandoff(*incoming) || !target_available() || session->revoking ||
      session->wait_cancel.Token().IsCancelled()) {
    respond(Error{ErrorCode::kCancelled, "client handoff target changed"});
    co_return;
  }
  if (!record && !session->pending_operation &&
      session->record.phase == AccountSessionPhase::kOffline) {
    DropIncomingClientHandoff(entity_id);
    client_sessions_.erase(entity_id);
    respond(record.Error());
    co_return;
  }
  if (record && *record == prepared) {
    auto operation = session->pending_operation;
    if (!operation) {
      auto created = MakeSessionOperation(session->account_dbid, session->account_type_id, *record,
                                          session->owner, AccountSessionAction::kAccept);
      if (created)
        operation = *created;
      else
        record = created.Error();
    }
    if (operation)
      record =
          co_await ApplyClientSessionOperation(session, *operation, session->wait_cancel.Token());
  }
  if (record && (!MatchesHandoff(*record, prepared) ||
                 (record->phase != AccountSessionPhase::kAccepted &&
                  record->phase != AccountSessionPhase::kAwaitingClient)))
    record = Error{ErrorCode::kPermissionDenied, "client handoff database state changed"};
  if (record && (!IsCurrentIncomingClientHandoff(*incoming) || !target_available() ||
                 session->wait_cancel.Token().IsCancelled() || session->revoking))
    record = Error{ErrorCode::kCommitUnknown, "client handoff target changed after confirmation"};
  if (record) {
    session->record = *record;
    if (session->pending_operation &&
        session->pending_operation->action == AccountSessionAction::kAccept)
      session->pending_operation.reset();
  }
  respond(std::move(record));
}

void BaseApp::OnAwaitClientHandoff(IntrusivePtr<Channel> channel,
                                   const baseapp::PrepareClientHandoff& request) {
  const auto entity_id = request.expected.target.entity_id;
  const auto found = client_sessions_.find(entity_id);
  if (found == client_sessions_.end() || found->second->account_dbid != request.account_dbid ||
      found->second->account_type_id != request.account_type_id) {
    ReplyToHandoff(*channel, request,
                   Error{ErrorCode::kReceiverGone, "client handoff target no longer tracked"});
    return;
  }
  const auto& session = found->second;
  const auto* proxy = entity_mgr_.FindProxy(entity_id);
  if (proxy && proxy->HasClient() && !session->frozen && !session->busy && !session->revoking &&
      session->record.phase == AccountSessionPhase::kActive &&
      session->handoff_source == request.expected.owner &&
      session->handoff_prepared_version == request.expected.version &&
      session->record.owner == request.expected.target &&
      session->record.handoff_id == request.expected.handoff_id &&
      session->record.version == request.expected.version + 3) {
    ReplyToHandoff(*channel, request, session->record);
    return;
  }
  const auto incoming = incoming_client_handoffs_.find(entity_id);
  if (incoming != incoming_client_handoffs_.end() && incoming->second->waiter &&
      incoming->second->request.expected == request.expected &&
      incoming->second->request.username == request.username &&
      incoming->second->waiter->request.request_id == request.request_id &&
      incoming->second->waiter->channel.get() == channel.get())
    return;
  if (incoming == incoming_client_handoffs_.end() ||
      incoming->second->request.expected != request.expected ||
      incoming->second->request.username != request.username || incoming->second->waiter ||
      session->revoking || session->wait_cancel.Token().IsCancelled()) {
    ReplyToHandoff(*channel, request,
                   Error{ErrorCode::kCommitUnknown, "client handoff confirmation unavailable"});
    return;
  }
  incoming->second->waiter.emplace(IncomingClientHandoff::Waiter{std::move(channel), request});
}

auto BaseApp::TryAuthenticateClientHandoff(Channel& channel, const baseapp::Authenticate& request)
    -> bool {
  if (request.session_key.IsZero()) return false;
  auto hash = Sha256(std::as_bytes(std::span{request.session_key.bytes}));
  if (!hash) {
    baseapp::AuthenticateResult reply;
    reply.error = "session_credential_unavailable";
    (void)channel.SendMessage(reply);
    return true;
  }
  const auto found = incoming_handoff_credentials_.find(CredentialIndex(*hash));
  if (found == incoming_handoff_credentials_.end()) return false;
  const auto incoming = incoming_client_handoffs_.at(found->second);
  auto* proxy = entity_mgr_.FindProxy(found->second);
  if (!IsCurrentIncomingClientHandoff(*incoming) || incoming->session->busy ||
      incoming->authenticating || incoming->expiring || incoming->session->revoking ||
      incoming->session->wait_cancel.Token().IsCancelled() || !proxy || proxy->IsPendingDestroy() ||
      proxy->HasClient()) {
    baseapp::AuthenticateResult reply;
    reply.error = "session_confirmation_pending";
    (void)channel.SendMessage(reply);
    return true;
  }
  incoming->authenticating = true;
  AuthenticateClientHandoff(IntrusivePtr<Channel>{&channel}, request, incoming);
  return true;
}

auto BaseApp::AuthenticateClientHandoff(IntrusivePtr<Channel> channel,
                                        baseapp::Authenticate request,
                                        std::shared_ptr<IncomingClientHandoff> incoming)
    -> FireAndForget {
  auto session = incoming->session;
  Result<AccountSession> record{Error{ErrorCode::kCancelled}};
  if (session->pending_operation) {
    record = co_await ApplyClientSessionOperation(session, *session->pending_operation,
                                                  session->wait_cancel.Token());
  } else {
    session->busy = true;
    record = co_await QueryAccountSession(session->account_dbid, session->account_type_id, {},
                                          session->wait_cancel.Token());
    session->busy = false;
  }
  auto target_available = [&] {
    auto* proxy = entity_mgr_.FindProxy(session->owner.entity_id);
    return proxy && !proxy->IsPendingDestroy() && !proxy->HasClient() &&
           (proxy->GetSessionKey().IsZero() || proxy->GetSessionKey() == request.session_key) &&
           !session->wait_cancel.Token().IsCancelled() && !session->revoking;
  };
  if (record && IsCurrentIncomingClientHandoff(*incoming) && target_available() &&
      MatchesHandoff(*record, incoming->request.expected) &&
      record->phase == AccountSessionPhase::kAwaitingClient) {
    auto operation = MakeSessionOperation(session->account_dbid, session->account_type_id, *record,
                                          session->owner, AccountSessionAction::kComplete);
    if (operation)
      record =
          co_await ApplyClientSessionOperation(session, *operation, session->wait_cancel.Token());
    else
      record = operation.Error();
  }
  auto* proxy = entity_mgr_.FindProxy(session->owner.entity_id);
  const bool confirmed = record && record->phase == AccountSessionPhase::kActive &&
                         record->owner == session->owner &&
                         record->handoff_id == incoming->request.expected.handoff_id;
  const bool bindable = IsCurrentIncomingClientHandoff(*incoming) && target_available();
  if (confirmed && bindable && !channel->IsCondemned() &&
      entity_mgr_.AssignSessionKey(session->owner.entity_id, request.session_key)) {
    session->record = *record;
    FinishClientAuthenticate(*channel, request);
    if (proxy->HasClient()) {
      RegisterAccountOwner(incoming->request.username, proxy->EntityId());
      session->handoff_source = incoming->request.expected.owner;
      session->handoff_prepared_version = incoming->request.expected.version;
      session->frozen = false;
      DropIncomingClientHandoff(proxy->EntityId());
    }
  } else if (!channel->IsCondemned()) {
    ++auth_fail_total_;
    baseapp::AuthenticateResult reply;
    reply.error = record ? "client_handoff_not_confirmed"
                         : std::format("client_handoff_{}", ErrorCodeName(record.Error().Code()));
    (void)channel->SendMessage(reply);
  }
  incoming->authenticating = false;
}

void BaseApp::DropIncomingClientHandoff(EntityID entity_id) {
  const auto found = incoming_client_handoffs_.find(entity_id);
  if (found == incoming_client_handoffs_.end()) return;
  const auto& incoming = found->second;
  if (incoming->waiter) {
    const auto& waiter = *incoming->waiter;
    const auto* proxy = entity_mgr_.FindProxy(entity_id);
    const auto& session = incoming->session;
    if (proxy && proxy->HasClient() && !session->frozen &&
        session->record.phase == AccountSessionPhase::kActive &&
        session->record.owner == incoming->request.expected.target &&
        session->record.handoff_id == incoming->request.expected.handoff_id)
      ReplyToHandoff(*waiter.channel, waiter.request, session->record);
    else
      ReplyToHandoff(*waiter.channel, waiter.request,
                     Error{ErrorCode::kCommitUnknown, "client handoff was not authenticated"});
  }
  incoming_handoff_credentials_.erase(found->second->credential_hash);
  incoming_client_handoffs_.erase(found);
  client_transfer_targets_.erase(entity_id);
}

void BaseApp::SweepIncomingClientHandoffs() {
  std::vector<std::shared_ptr<IncomingClientHandoff>> expired;
  const auto now = Clock::now();
  for (const auto& [_, incoming] : incoming_client_handoffs_) {
    if (!incoming->authenticating && !incoming->expiring && !incoming->session->busy &&
        !incoming->session->revoking && now - incoming->checked_at >= std::chrono::seconds(20)) {
      incoming->checked_at = now;
      incoming->expiring = true;
      expired.push_back(incoming);
    }
  }
  for (auto& incoming : expired) ExpireIncomingClientHandoff(std::move(incoming));
}

auto BaseApp::ExpireIncomingClientHandoff(std::shared_ptr<IncomingClientHandoff> incoming)
    -> FireAndForget {
  auto session = incoming->session;
  session->busy = true;
  auto record = co_await QueryAccountSession(session->account_dbid, session->account_type_id, {});
  session->busy = false;
  if (record && IsCurrentIncomingClientHandoff(*incoming)) {
    if (MatchesHandoff(*record, incoming->request.expected) ||
        (record->phase == AccountSessionPhase::kActive && record->owner == session->owner &&
         record->handoff_id == incoming->request.expected.handoff_id)) {
      (void)co_await ReleaseClientSession(session->owner.entity_id, record->version);
    } else {
      const auto entity_id = session->owner.entity_id;
      DropIncomingClientHandoff(entity_id);
      client_sessions_.erase(entity_id);
      (void)entity_mgr_.ClearSessionKey(entity_id);
    }
  }
  incoming->expiring = false;
}

}  // namespace atlas
