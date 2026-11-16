#include <algorithm>

#include "baseapp.h"
#include "coro/rpc_call.h"
#include "coro/scope_guard.h"
#include "foundation/log.h"
#include "platform/secure_random.h"

namespace atlas {

BaseApp::BaseApp(EventDispatcher& dispatcher, NetworkInterface& internal_network,
                 NetworkInterface& external_network)
    : EntityApp(dispatcher, internal_network),
      external_network_(external_network),
      rpc_registry_(dispatcher),
      cellapp_peers_(internal_network) {}

BaseApp::~BaseApp() {
  recovery_cancel_.RequestCancellation();
  rpc_registry_.Shutdown();
}

auto BaseApp::MakeSessionOperation(DatabaseID dbid, uint16_t type_id, const AccountSession& record,
                                   const AccountSessionOwner& owner, AccountSessionAction action)
    -> Result<AccountSessionOperation> {
  AccountSessionOperation operation;
  if (auto random = FillSecureRandom(operation.id); !random) return random.Error();
  operation.account_dbid = dbid;
  operation.account_type_id = type_id;
  operation.expected_version = record.version;
  operation.actor = owner;
  operation.action = action;
  if (action >= AccountSessionAction::kPrepare && action <= AccountSessionAction::kAbort) {
    operation.handoff_id = record.handoff_id;
    operation.credential_hash = record.credential_hash;
  }
  return operation;
}

namespace {

void RejectAuthentication(Channel& channel, std::string_view error) {
  if (channel.IsCondemned()) return;
  baseapp::AuthenticateResult reply;
  reply.error = error;
  (void)channel.SendMessage(reply);
}

}  // namespace

auto BaseApp::QueryAccountSession(DatabaseID dbid, uint16_t type_id,
                                  std::optional<AccountSessionOperation> operation,
                                  CancellationToken token) -> Task<Result<AccountSession>> {
  if (dbid <= kInvalidDBID || type_id == 0 || Config().process_instance == 0)
    co_return Error{ErrorCode::kInvalidArgument, "invalid account session query"};
  if (operation && (operation->account_dbid != dbid || operation->account_type_id != type_id))
    co_return Error{ErrorCode::kInvalidArgument, "account session operation target mismatch"};
  if (token.IsCancelled()) co_return Error{ErrorCode::kCancelled, "session query cancelled"};
  if (rpc_registry_.IsStopping()) co_return Error{ErrorCode::kReceiverGone, "BaseApp is stopping"};
  auto* route = ResolveDbAppChannel(dbid);
  if (route == nullptr || route->IsCondemned())
    co_return Error{ErrorCode::kReceiverGone, "account session DBApp unavailable"};
  IntrusivePtr<Channel> channel{route};
  dbapp::AccountSessionRequest request;
  request.request_id = rpc_registry_.AllocateRequestId();
  if (request.request_id == 0)
    co_return Error{ErrorCode::kOutOfRange, "account session request IDs exhausted"};
  request.process_instance = Config().process_instance;
  request.shard_table_version = dbapp_shard_table_version_;
  request.account_dbid = dbid;
  request.account_type_id = type_id;
  request.operation = std::move(operation);
  auto reply = co_await RpcCall<dbapp::AccountSessionReply>(
      rpc_registry_, *channel, request, std::chrono::seconds(5), std::move(token));
  if (!reply) {
    const auto code = reply.Error().Code();
    if (request.operation && code != ErrorCode::kAlreadyExists && code != ErrorCode::kRateLimited &&
        code != ErrorCode::kWouldBlock)
      co_return Error{ErrorCode::kCommitUnknown, "account session mutation was not confirmed"};
    co_return reply.Error();
  }
  const auto expected_id = request.operation ? request.operation->id : std::array<std::byte, 16>{};
  if (reply->requester_instance != request.process_instance || reply->account_dbid != dbid ||
      reply->account_type_id != type_id || reply->operation_id != expected_id)
    co_return Error{request.operation ? ErrorCode::kCommitUnknown : ErrorCode::kInvalidArgument,
                    "account session reply correlation mismatch"};
  if (reply->status != ErrorCode::kNone) co_return Error{reply->status, reply->error};
  co_return reply->session;
}

void BaseApp::OnAccountSessionReply(Channel* channel, const dbapp::AccountSessionReply& reply) {
  if (reply.requester_instance != Config().process_instance) return;
  BinaryWriter payload;
  reply.Serialize(payload);
  (void)rpc_registry_.TryDispatch(dbapp::AccountSessionReply::Descriptor().id, payload.Data(),
                                  channel);
}

auto BaseApp::HasFrozenClientSession(EntityID entity_id) const -> bool {
  const auto found = client_sessions_.find(entity_id);
  return found != client_sessions_.end() && found->second->frozen;
}

auto BaseApp::ApplyClientSessionOperation(std::shared_ptr<ClientSession> session,
                                          AccountSessionOperation operation,
                                          CancellationToken token) -> Task<Result<AccountSession>> {
  if (session->busy)
    co_return Error{session->pending_operation ? ErrorCode::kCommitUnknown : ErrorCode::kWouldBlock,
                    "account session operation already pending"};
  if (session->pending_operation && session->pending_operation->id != operation.id)
    co_return Error{ErrorCode::kCommitUnknown, "previous account session operation is unconfirmed"};
  const bool resolving = session->pending_operation.has_value();
  session->busy = true;
  session->pending_operation = operation;
  auto result = co_await QueryAccountSession(session->account_dbid, session->account_type_id,
                                             operation, token);
  if (!result && resolving && result.Error().Code() != ErrorCode::kCommitUnknown) {
    bool superseded = false;
    if (result.Error().Code() == ErrorCode::kPermissionDenied) {
      auto current =
          co_await QueryAccountSession(session->account_dbid, session->account_type_id, {}, token);
      superseded = current && current->version > operation.expected_version;
    }
    if (!superseded)
      result =
          Error{ErrorCode::kCommitUnknown, "original account session operation is unconfirmed"};
  }
  session->busy = false;
  if (result) {
    session->record = *result;
    if (operation.action == AccountSessionAction::kMoveLocal) session->owner = result->owner;
  }
  if (result || result.Error().Code() != ErrorCode::kCommitUnknown)
    session->pending_operation.reset();
  co_return result;
}

void BaseApp::OnClientAuthenticate(Channel& channel, const baseapp::Authenticate& request) {
  if (IsShuttingDown()) {
    RejectAuthentication(channel, "server_stopping");
    return;
  }
  if (TryAuthenticateClientHandoff(channel, request)) return;
  auto* proxy = entity_mgr_.FindProxyBySession(request.session_key);
  if (proxy == nullptr) {
    ++auth_fail_total_;
    RejectAuthentication(channel, "invalid_session");
    return;
  }
  const EntityID entity_id = proxy->EntityId();
  auto existing = client_sessions_.find(entity_id);
  if (existing != client_sessions_.end()) {
    const auto& session = existing->second;
    if (session->busy || session->revoking || session->wait_cancel.Token().IsCancelled()) {
      RejectAuthentication(channel, "session_confirmation_pending");
      return;
    }
    if (!session->frozen && session->record.phase == AccountSessionPhase::kActive &&
        session->record.owner == session->owner && !session->pending_operation &&
        (!request.supports_recovery || session->recovery_credential.IsPresent())) {
      FinishClientAuthenticate(channel, request);
      return;
    }
    AuthenticateClientSession(IntrusivePtr<Channel>{&channel}, request, session);
    return;
  }
  const auto prepared_id = prepared_login_requests_by_entity_.find(entity_id);
  if (prepared_id == prepared_login_requests_by_entity_.end()) {
    RejectAuthentication(channel, "account_session_not_prepared");
    return;
  }
  const auto prepared = prepared_login_entities_.find(prepared_id->second);
  if (prepared == prepared_login_entities_.end() || !prepared->second.checkout_owner ||
      prepared->second.dbid <= kInvalidDBID) {
    RejectAuthentication(channel, "account_session_checkout_missing");
    return;
  }
  const auto& checkout = *prepared->second.checkout_owner;
  auto session = std::make_shared<ClientSession>();
  session->account_dbid = prepared->second.dbid;
  session->account_type_id = prepared->second.type_id;
  session->prepared_request_id = prepared_id->second;
  session->owner = {checkout.base_addr, entity_id, checkout.process_instance};
  client_sessions_.emplace(entity_id, session);
  AuthenticateClientSession(IntrusivePtr<Channel>{&channel}, request, std::move(session));
}

auto BaseApp::AuthenticateClientSession(IntrusivePtr<Channel> channel,
                                        baseapp::Authenticate request,
                                        std::shared_ptr<ClientSession> session) -> FireAndForget {
  Result<AccountSession> record{Error{ErrorCode::kInternalError}};
  if (session->pending_operation) {
    record = co_await ApplyClientSessionOperation(session, *session->pending_operation,
                                                  session->wait_cancel.Token());
  } else {
    session->busy = true;
    record = co_await QueryAccountSession(session->account_dbid, session->account_type_id, {},
                                          session->wait_cancel.Token());
    session->busy = false;
    if (record) session->record = *record;
  }
  if (record && record->phase == AccountSessionPhase::kOffline &&
      !session->wait_cancel.Token().IsCancelled()) {
    const auto prepared = prepared_login_entities_.find(session->prepared_request_id);
    if (prepared == prepared_login_entities_.end() || !prepared->second.checkout_owner) {
      record = Error{ErrorCode::kCancelled, "prepared account session expired"};
    } else {
      auto operation = MakeSessionOperation(session->account_dbid, session->account_type_id,
                                            *record, session->owner, AccountSessionAction::kClaim);
      if (!operation) {
        record = operation.Error();
      } else {
        operation->claim_checkout = prepared->second.checkout_owner;
        record =
            co_await ApplyClientSessionOperation(session, *operation, session->wait_cancel.Token());
      }
    }
  }
  const auto found = client_sessions_.find(session->owner.entity_id);
  auto* proxy = entity_mgr_.FindProxyBySession(request.session_key);
  const bool current = found != client_sessions_.end() && found->second == session &&
                       proxy != nullptr && proxy->EntityId() == session->owner.entity_id &&
                       !proxy->IsPendingDestroy();
  if (!record || record->phase != AccountSessionPhase::kActive || record->owner != session->owner ||
      !current || session->wait_cancel.Token().IsCancelled()) {
    ++auth_fail_total_;
    RejectAuthentication(*channel, "account_session_confirmation_failed");
    co_return;
  }
  session->supports_recovery = request.supports_recovery;
  if (request.supports_recovery) {
    auto issued = co_await IssueClientRecoveryCredential(session);
    if (!issued || session->wait_cancel.Token().IsCancelled()) {
      ++auth_fail_total_;
      RejectAuthentication(*channel, "recovery_credential_unconfirmed");
      co_return;
    }
  }
  session->frozen = false;
  if (!channel->IsCondemned()) FinishClientAuthenticate(*channel, request);
}

auto BaseApp::IssueClientRecoveryCredential(std::shared_ptr<ClientSession> session,
                                            AccountSessionAction action, Sha256Digest proof,
                                            std::optional<CheckoutInfo> account_checkout)
    -> Task<Result<void>> {
  if (session->busy)
    co_return Error{ErrorCode::kWouldBlock, "recovery credential confirmation pending"};
  auto* entity = entity_mgr_.Find(session->owner.entity_id);
  const auto current = client_sessions_.find(session->owner.entity_id);
  if (!entity || entity->IsPendingDestroy() || current == client_sessions_.end() ||
      current->second != session || session->record.phase != AccountSessionPhase::kActive ||
      (action != AccountSessionAction::kClaimRecovery && session->record.owner != session->owner))
    co_return Error{ErrorCode::kPermissionDenied, "client session is not recoverable"};
  RecoveryCredential credential;
  credential.account_dbid = session->account_dbid;
  credential.account_type_id = session->account_type_id;
  if (auto random = FillSecureRandom(credential.value); !random) co_return random.Error();
  if (!credential.IsPresent()) co_return Error{ErrorCode::kIoError, "empty recovery randomness"};
  auto digest = Sha256(credential.value);
  if (!digest) co_return digest.Error();
  auto operation = MakeSessionOperation(session->account_dbid, session->account_type_id,
                                        session->record, session->owner, action);
  if (!operation) co_return operation.Error();
  operation->recovery.credential_hash = *digest;
  operation->recovery.entity_dbid = entity->Dbid();
  operation->recovery.entity_type_id = entity->TypeId();
  operation->recovery_proof = proof;
  if (action == AccountSessionAction::kClaimRecovery) {
    operation->claim_checkout = account_checkout;
    operation->recovery_checkout = entity->CheckoutOwner();
  }
  auto saved =
      co_await ApplyClientSessionOperation(session, *operation, session->wait_cancel.Token());
  if (!saved) co_return saved.Error();
  const auto still_current = client_sessions_.find(session->owner.entity_id);
  if (still_current == client_sessions_.end() || still_current->second != session ||
      session->wait_cancel.Token().IsCancelled())
    co_return Error{ErrorCode::kCancelled, "recovery session no longer current"};
  if (saved->recovery.credential_hash != *digest || saved->owner != session->owner)
    co_return Error{ErrorCode::kCommitUnknown, "recovery grant confirmation mismatch"};
  credential.expires_at_ms = saved->recovery.expires_at_ms;
  session->recovery_credential = credential;
  session->recovery_deadline = Clock::now() + Milliseconds(kSessionRecoveryLifetimeMs);
  co_return Result<void>{};
}

auto BaseApp::ReleaseClientSession(EntityID entity_id, std::optional<uint64_t> expected_version)
    -> Task<Result<void>> {
  const auto found = client_sessions_.find(entity_id);
  if (found == client_sessions_.end()) co_return Result<void>{};
  auto session = found->second;
  if (session->revoking)
    co_return Error{ErrorCode::kWouldBlock, "account session revocation already pending"};
  session->revoking = true;
  session->frozen = true;
  session->wait_cancel.RequestCancellation();
  if (session->pending_operation) {
    auto resolved = co_await ApplyClientSessionOperation(session, *session->pending_operation);
    if (!resolved && resolved.Error().Code() == ErrorCode::kCommitUnknown) {
      session->revoking = false;
      co_return resolved.Error();
    }
  }
  auto record = co_await QueryAccountSession(session->account_dbid, session->account_type_id, {});
  if (record && expected_version && record->phase != AccountSessionPhase::kOffline &&
      record->version != *expected_version) {
    session->revoking = false;
    co_return Error{ErrorCode::kPermissionDenied, "account session revocation version changed"};
  }
  const auto incoming = incoming_client_handoffs_.find(entity_id);
  if (record && incoming != incoming_client_handoffs_.end() &&
      record->phase >= AccountSessionPhase::kPreparing && record->target == session->owner &&
      record->handoff_id == incoming->second->request.expected.handoff_id) {
    auto operation = MakeSessionOperation(session->account_dbid, session->account_type_id, *record,
                                          session->owner, AccountSessionAction::kAbort);
    if (operation) {
      record = co_await ApplyClientSessionOperation(session, *operation);
    } else {
      record = operation.Error();
    }
  } else if (record && record->phase != AccountSessionPhase::kOffline &&
             record->owner == session->owner) {
    auto operation = MakeSessionOperation(session->account_dbid, session->account_type_id, *record,
                                          session->owner, AccountSessionAction::kRelease);
    if (operation)
      record = co_await ApplyClientSessionOperation(session, *operation);
    else
      record = operation.Error();
  }
  session->revoking = false;
  if (!record) co_return record.Error();
  auto current = client_sessions_.find(entity_id);
  if (current == client_sessions_.end() || current->second != session)
    co_return Error{ErrorCode::kCancelled, "account session changed during revocation"};
  client_transfer_targets_.erase(session->transfer_target);
  DropIncomingClientHandoff(entity_id);
  client_sessions_.erase(current);
  (void)entity_mgr_.ClearSessionKey(entity_id);
  UnbindClient(entity_id);
  co_return Result<void>{};
}

auto BaseApp::ReleaseClientSessionThen(EntityID entity_id, std::function<void(bool)> complete)
    -> FireAndForget {
  auto result = co_await ReleaseClientSession(entity_id);
  if (!result)
    ATLAS_LOG_ERROR("BaseApp: account session release failed entity={} status={}: {}", entity_id,
                    ErrorCodeName(result.Error().Code()), result.Error().Message());
  complete(result.HasValue());
}

auto BaseApp::ReloginAfterSessionRelease(EntityID entity_id, PendingLogin pending)
    -> FireAndForget {
  auto released = co_await ReleaseClientSession(entity_id);
  if (!released) {
    FailPendingPrepareLogin(pending, "account_session_release_failed");
    FinishLoginFlow(pending.dbid);
    co_return;
  }
  DispatchPrepareLogin(std::move(pending));
}

auto BaseApp::MoveClientSession(EntityID source, EntityID destination) -> Task<Result<void>> {
  auto* from = entity_mgr_.FindProxy(source);
  auto* to = entity_mgr_.FindProxy(destination);
  if (from == nullptr || to == nullptr || from->IsPendingDestroy() || to->IsPendingDestroy() ||
      !from->HasClient() || from->GetSessionKey().IsZero() ||
      (source != destination && (to->HasClient() || !to->GetSessionKey().IsZero())))
    co_return Error{ErrorCode::kInvalidArgument, "client transfer target unavailable"};
  if (source == destination) co_return Result<void>{};
  const auto found = client_sessions_.find(source);
  if (found == client_sessions_.end() || found->second->frozen || found->second->busy ||
      found->second->pending_operation || client_sessions_.contains(destination) ||
      client_transfer_targets_.contains(destination))
    co_return Error{ErrorCode::kPermissionDenied, "client session is not transferable"};
  auto session = found->second;
  session->frozen = true;
  session->transfer_target = destination;
  client_transfer_targets_.insert(destination);
  const auto key = from->GetSessionKey();
  auto operation =
      MakeSessionOperation(session->account_dbid, session->account_type_id, session->record,
                           session->owner, AccountSessionAction::kMoveLocal);
  if (!operation) {
    client_transfer_targets_.erase(destination);
    session->transfer_target = kInvalidEntityID;
    session->frozen = false;
    co_return operation.Error();
  }
  operation->target = session->owner;
  operation->target.entity_id = destination;
  auto moved =
      co_await ApplyClientSessionOperation(session, *operation, session->wait_cancel.Token());
  if (!moved) {
    if (moved.Error().Code() != ErrorCode::kCommitUnknown) {
      auto current =
          co_await QueryAccountSession(session->account_dbid, session->account_type_id, {});
      if (current && current->phase == AccountSessionPhase::kActive &&
          current->owner == session->owner && current->version == session->record.version &&
          !session->wait_cancel.Token().IsCancelled()) {
        client_transfer_targets_.erase(destination);
        session->transfer_target = kInvalidEntityID;
        session->frozen = false;
      }
    }
    co_return moved.Error();
  }
  from = entity_mgr_.FindProxy(source);
  to = entity_mgr_.FindProxy(destination);
  auto current = client_sessions_.find(source);
  if (current == client_sessions_.end() || current->second != session || from == nullptr ||
      to == nullptr || from->IsPendingDestroy() || to->IsPendingDestroy() || !from->HasClient() ||
      from->GetSessionKey() != key || to->HasClient() || !to->GetSessionKey().IsZero() ||
      session->wait_cancel.Token().IsCancelled()) {
    co_return Error{ErrorCode::kCommitUnknown,
                    "client transfer binding changed after confirmation"};
  }
  auto installed = DoGiveClientToLocal(source, destination);
  if (!to->HasClient())
    co_return Error{ErrorCode::kCommitUnknown, "client transfer binding could not be installed"};
  client_sessions_.erase(source);
  client_sessions_.emplace(destination, session);
  client_transfer_targets_.erase(destination);
  session->transfer_target = kInvalidEntityID;
  if (!installed)
    co_return Error{ErrorCode::kCommitUnknown, "client transfer notification was not confirmed"};
  if (session->supports_recovery) {
    auto issued = co_await IssueClientRecoveryCredential(session);
    if (!issued) co_return issued.Error();
    auto* client = ResolveClientChannel(destination);
    if (!client) co_return Error{ErrorCode::kReceiverGone, "client transfer channel unavailable"};
    if (auto sent = client->SendMessage(
            baseapp::RecoveryCredentialUpdate{destination, session->recovery_credential});
        !sent)
      co_return sent.Error();
  }
  session->frozen = false;
  co_return Result<void>{};
}

auto BaseApp::StartClientTransfer(EntityID source, EntityID destination, uint64_t request_id,
                                  AccountSessionOwner target) -> RpcSendResult {
  if (IsShuttingDown() || rpc_registry_.IsStopping()) return RpcSendResult::kStopping;
  if ((target.base_addr != Address{} || target.process_instance != 0) &&
      !IsValidSessionOwner(target))
    return RpcSendResult::kInvalidTarget;
  const auto reply_id = msg_id::Id(msg_id::Common::kEntityRpcReply);
  if (source == kInvalidEntityID || request_id == 0 ||
      !rpc_registry_.BindChannel(reply_id, request_id, nullptr))
    return RpcSendResult::kInvalidTarget;
  const auto reply = rpc_registry_.RegisterReply(source, request_id, nullptr);
  if (reply == 0) return RpcSendResult::kOverloaded;
  CompleteClientTransfer(source, destination, request_id, reply, target);
  return RpcSendResult::kQueued;
}

auto BaseApp::CompleteClientTransfer(EntityID source, EntityID destination, uint64_t request_id,
                                     intptr_t reply, AccountSessionOwner target) -> FireAndForget {
  auto result = target.process_instance == 0 ? co_await MoveClientSession(source, destination)
                                             : co_await MoveRemoteClientSession(source, target);
  BinaryWriter body;
  if (result) {
    body.Write(uint8_t{1});
    rpc_registry_.SendReply(reply, request_id, 0, body.Data());
  } else {
    body.WriteString(result.Error().Message());
    const auto code = result.Error().Code() == ErrorCode::kCommitUnknown ? -8 : -4;
    rpc_registry_.SendReply(reply, request_id, code, body.Data());
  }
}

void BaseApp::BeginLogoffPersist(EntityID entity_id, DatabaseID dbid, uint16_t type_id,
                                 uint32_t continuation_request_id) {
  if (client_sessions_.contains(entity_id)) {
    ReleaseClientSessionThen(
        entity_id, [this, entity_id, dbid, type_id, continuation_request_id](bool success) {
          if (success) {
            BeginLogoffPersist(entity_id, dbid, type_id, continuation_request_id);
          } else {
            if (continuation_request_id != 0)
              FailPendingForceLogoff(continuation_request_id, "account_session_release_failed");
            FailDeferredPrepareLogins(entity_id, "account_session_release_failed");
            FlushRemoteForceLogoffAcks(entity_id, false);
            FinishLoginFlow(dbid);
          }
        });
    return;
  }
  auto write =
      std::find_if(pending_write_to_db_.begin(), pending_write_to_db_.end(),
                   [entity_id](const auto& entry) { return entry.second.entity_id == entity_id; });
  if (write != pending_write_to_db_.end()) write->second.logoff_after_write = true;
  if (logoff_entities_in_flight_.contains(entity_id) || write != pending_write_to_db_.end()) {
    ATLAS_LOG_WARNING("BaseApp: logoff blocked by pending persistence for entity={}", entity_id);
    if (continuation_request_id != 0) {
      FailPendingForceLogoff(continuation_request_id, "persistence_pending");
    }
    FlushRemoteForceLogoffAcks(entity_id, false);
    return;
  }
  if (entity_id == kInvalidEntityID) {
    if (continuation_request_id != 0)
      ContinueLoginAfterForceLogoff(continuation_request_id);
    else
      FinishLoginFlow(dbid);
    return;
  }

  auto* ent = entity_mgr_.Find(entity_id);
  if (!ent || ent->Dbid() != dbid || ent->IsPendingDestroy()) {
    if (continuation_request_id != 0)
      ContinueLoginAfterForceLogoff(continuation_request_id);
    else
      FinishLoginFlow(dbid);
    return;
  }

  std::vector<std::byte> blob;
  if (!CaptureEntitySnapshot(entity_id, blob)) {
    FinishShutdownSave(entity_id, false, "snapshot failed");
    if (continuation_request_id != 0)
      FailPendingForceLogoff(continuation_request_id, "force_logoff_snapshot_failed");
    FlushRemoteForceLogoffAcks(entity_id, false);
    FinishLoginFlow(dbid);
    return;
  }

  dbapp::WriteEntity msg;
  std::ranges::copy(EntityDefRegistry::Instance().Digest(), msg.entity_def_digest.begin());
  if (auto generated = FillSecureRandom(msg.operation_id); !generated) {
    FinishShutdownSave(entity_id, false, "operation ID generation failed");
    if (continuation_request_id != 0)
      FailPendingForceLogoff(continuation_request_id, "operation_id_generation_failed");
    FailDeferredPrepareLogins(entity_id, "operation_id_generation_failed");
    FlushRemoteForceLogoffAcks(entity_id, false);
    FinishLoginFlow(dbid);
    return;
  }
  msg.flags = WriteFlags::kExplicitDbid | WriteFlags::kLogOff;
  msg.type_id = type_id;
  msg.dbid = dbid;
  msg.entity_id = entity_id;
  msg.owner = ent->CheckoutOwner().value_or(CheckoutInfo{});
  msg.request_id = AllocatePrepareRequestId();
  msg.shard_table_version = dbapp_shard_table_version_;
  msg.blob = std::move(blob);

  PendingLogoffWrite pending;
  pending.continuation_request_id = continuation_request_id;
  pending.entity_id = entity_id;
  pending.dbid = dbid;
  pending.type_id = type_id;
  pending.message = std::move(msg);
  pending.created_at = Clock::now();

  const uint32_t kRequestId = pending.message.request_id;
  logoff_entities_in_flight_.insert(entity_id);
  auto [it, inserted] = pending_logoff_writes_.emplace(kRequestId, std::move(pending));
  if (!inserted) {
    FinishShutdownSave(entity_id, false, "duplicate logoff write request ID");
    logoff_entities_in_flight_.erase(entity_id);
    ATLAS_LOG_ERROR("BaseApp: duplicate logoff write request_id={} entity_id={}", kRequestId,
                    entity_id);
    return;
  }

  if (!SendPendingLogoffWriteToDb(kRequestId, it->second)) {
    RequestDbAppShardTable();
  }
}

void BaseApp::BeginForceLogoffPersist(uint32_t force_request_id, EntityID entity_id) {
  if (client_sessions_.contains(entity_id)) {
    ReleaseClientSessionThen(entity_id, [this, force_request_id, entity_id](bool success) {
      if (success)
        BeginForceLogoffPersist(force_request_id, entity_id);
      else
        FailPendingForceLogoff(force_request_id, "account_session_release_failed");
    });
    return;
  }
  auto* ent = entity_mgr_.Find(entity_id);
  if (!ent || ent->Dbid() == kInvalidDBID) {
    if (!FinalizeForceLogoff(entity_id)) {
      FailPendingForceLogoff(force_request_id, "force_logoff_destroy_failed");
      auto pending_it = pending_force_logoffs_.find(force_request_id);
      if (pending_it != pending_force_logoffs_.end()) {
        FinishLoginFlow(pending_it->second.dbid);
      }
      return;
    }
    ContinueLoginAfterForceLogoff(force_request_id);
    return;
  }

  BeginLogoffPersist(entity_id, ent->Dbid(), ent->TypeId(), force_request_id);
}

void BaseApp::StartDisconnectLogoff(EntityID entity_id) {
  if (client_sessions_.contains(entity_id)) {
    ReleaseClientSessionThen(entity_id, [this, entity_id](bool success) {
      if (success)
        StartDisconnectLogoff(entity_id);
      else
        FinishShutdownSave(entity_id, false, "session release failed");
    });
    return;
  }
  ClearDetachedGrace(entity_id);

  auto* ent = entity_mgr_.Find(entity_id);
  if (!ent || ent->IsPendingDestroy()) {
    FinishShutdownSave(entity_id, true, {});
    return;
  }

  const DatabaseID kDbid = ent->Dbid();
  if (kDbid != kInvalidDBID) {
    active_login_dbids_.insert(kDbid);
  }

  if (kDbid == kInvalidDBID) {
    const bool finalized = FinalizeForceLogoff(entity_id);
    FinishShutdownSave(entity_id, finalized, "temporary entity teardown failed");
    return;
  }

  BeginLogoffPersist(entity_id, kDbid, ent->TypeId(), 0);
}

auto BaseApp::RollbackPreparedLoginEntity(uint32_t login_request_id) -> bool {
  auto it = prepared_login_entities_.find(login_request_id);
  if (it == prepared_login_entities_.end()) {
    return false;
  }
  if (auto session = client_sessions_.find(it->second.entity_id);
      session != client_sessions_.end()) {
    if (!session->second->revoking) {
      ReleaseClientSessionThen(it->second.entity_id, [this, login_request_id](bool success) {
        if (success) (void)RollbackPreparedLoginEntity(login_request_id);
      });
    }
    return true;
  }

  const PreparedLoginEntity kPrepared = it->second;
  prepared_login_entities_.erase(it);
  prepared_login_requests_by_entity_.erase(kPrepared.entity_id);

  auto* ent = entity_mgr_.Find(kPrepared.entity_id);
  if (!ent) {
    return true;
  }

  (void)NotifyManagedEntityDestroyed(kPrepared.entity_id, "prepare login cancel");
  ClearDetachedGrace(kPrepared.entity_id);
  UnbindClient(kPrepared.entity_id);
  (void)entity_mgr_.ClearSessionKey(kPrepared.entity_id);
  (void)entity_mgr_.AssignDbid(kPrepared.entity_id, kInvalidDBID);
  ent->MarkForDestroy();
  ReleaseCheckout(kPrepared.dbid, kPrepared.type_id, kPrepared.checkout_owner);
  return true;
}

}  // namespace atlas
