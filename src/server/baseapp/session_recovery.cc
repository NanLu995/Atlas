#include "baseapp.h"
#include "baseapp_messages.h"
#include "coro/scope_guard.h"
#include "db/account_session.h"
#include "entitydef/entity_def_registry.h"
#include "foundation/log.h"
#include "loginapp/login_messages.h"
#include "platform/sha256.h"
#include "server/await_machined_query.h"

namespace atlas {

auto BaseApp::RefreshPersistedRecoveryCredential(EntityID entity_id) -> FireAndForget {
  const auto local = client_sessions_.find(entity_id);
  const auto* entity = entity_mgr_.Find(entity_id);
  if (local == client_sessions_.end() || !entity || !local->second->supports_recovery ||
      local->second->record.recovery.entity_dbid == entity->Dbid())
    co_return;
  auto session = local->second;
  auto issued = co_await IssueClientRecoveryCredential(session);
  if (!issued) {
    ATLAS_LOG_WARNING("BaseApp: persisted recovery credential unconfirmed entity={} status={}",
                      entity_id, ErrorCodeName(issued.Error().Code()));
    co_return;
  }
  if (auto* client = ResolveClientChannel(entity_id))
    (void)client->SendMessage(
        baseapp::RecoveryCredentialUpdate{entity_id, session->recovery_credential});
}

auto BaseApp::OnClientLogout(IntrusivePtr<Channel> channel, baseapp::ClientLogout request)
    -> FireAndForget {
  baseapp::ClientLogoutResult reply;
  reply.request_id = request.request_id;
  const auto binding = client_entity_index_.find(channel->RemoteAddress());
  if (binding == client_entity_index_.end() || binding->second != request.entity_id ||
      ResolveClientChannel(request.entity_id) != channel.get() ||
      !client_sessions_.contains(request.entity_id)) {
    reply.status = ErrorCode::kPermissionDenied;
    reply.error = "logout_binding_mismatch";
  } else if (session_revocations_ >= 1024 || IsShuttingDown()) {
    reply.status = ErrorCode::kRateLimited;
    reply.error = "logout_unavailable";
  } else {
    ++session_revocations_;
    auto released = co_await ReleaseClientSession(request.entity_id);
    --session_revocations_;
    if (!released) {
      reply.status = released.Error().Code();
      reply.error = std::string(ErrorCodeName(reply.status));
    }
  }
  if (!channel->IsCondemned()) (void)channel->SendMessage(reply);
  if (reply.status == ErrorCode::kNone) {
    ClearDetachedGrace(request.entity_id);
    StartDisconnectLogoff(request.entity_id);
  }
}

auto BaseApp::OnPrepareResume(IntrusivePtr<Channel> channel, login::PrepareResume request)
    -> FireAndForget {
  auto respond = [&](Result<login::PrepareResumeResult> result) {
    login::PrepareResumeResult reply;
    if (result)
      reply = *result;
    else
      reply.error = result.Error().Message().substr(0, 512);
    reply.request_id = request.request_id;
    reply.requester_instance = request.requester_instance;
    if (!channel->IsCondemned()) (void)channel->SendMessage(reply);
  };
  if (IsShuttingDown() || session_recoveries_ >= 128) {
    respond(Error{ErrorCode::kRateLimited, "recovery_unavailable"});
    co_return;
  }
  ++session_recoveries_;
  ScopeGuard count_guard([this] { --session_recoveries_; });
  auto sources = co_await AwaitMachinedQuery(GetMachinedClient(), ProcessType::kLoginApp,
                                             recovery_cancel_.Token());
  if (!sources) {
    respond(sources.Error());
    co_return;
  }
  if (std::ranges::none_of(*sources, [&](const auto& process) {
        return process.internal_addr == channel->RemoteAddress() &&
               process.process_instance == request.requester_instance;
      })) {
    respond(Error{ErrorCode::kPermissionDenied, "unregistered_recovery_requester"});
    co_return;
  }
  if (channel->IsCondemned() || IsShuttingDown()) co_return;
  respond(co_await PrepareSessionRecovery(std::move(request)));
}

auto BaseApp::PrepareSessionRecovery(login::PrepareResume request)
    -> Task<Result<login::PrepareResumeResult>> {
  if (!recovery_accounts_.insert(request.account_dbid).second)
    co_return Error{ErrorCode::kWouldBlock, "account_recovery_already_pending"};
  ScopeGuard account_guard([this, dbid = request.account_dbid] { recovery_accounts_.erase(dbid); });
  if (!std::ranges::equal(request.entity_def_digest, EntityDefRegistry::Instance().Digest()))
    co_return Error{ErrorCode::kInvalidArgument, "def_mismatch"};
  auto record = co_await QueryAccountSession(request.account_dbid, request.account_type_id, {},
                                             recovery_cancel_.Token());
  if (!record) co_return record.Error();
  if (record->phase != AccountSessionPhase::kActive ||
      !HasSessionValue(record->recovery.credential_hash) ||
      !Sha256DigestEquals(record->recovery.credential_hash, request.proof))
    co_return Error{ErrorCode::kPermissionDenied, "invalid_recovery_credential_or_binding"};
  login::PrepareResumeResult reply;
  const auto peer = baseapp_peers_.find(record->owner.base_addr);
  if (record->owner.process_instance != Config().process_instance) {
    if (peer != baseapp_peers_.end() &&
        peer->second.process_instance == record->owner.process_instance) {
      reply.redirect = peer->second.internal_addr;
      co_return reply;
    }
    co_return co_await RecoverPersistentSession(std::move(request), *record);
  }
  if (peer == baseapp_peers_.end() || peer->second.external_addr.Port() == 0)
    co_return Error{ErrorCode::kReceiverGone, "recovery_public_address_unavailable"};
  const auto external_addr = peer->second.external_addr;
  const auto local = client_sessions_.find(record->owner.entity_id);
  auto* proxy = entity_mgr_.FindProxy(record->owner.entity_id);
  if (local == client_sessions_.end() || !proxy || proxy->IsPendingDestroy() ||
      local->second->owner != record->owner ||
      local->second->account_dbid != request.account_dbid ||
      local->second->account_type_id != request.account_type_id ||
      proxy->Dbid() != record->recovery.entity_dbid ||
      proxy->TypeId() != record->recovery.entity_type_id)
    co_return Error{ErrorCode::kPermissionDenied, "recovery_binding_unavailable"};
  auto session = local->second;
  if (session->busy || session->frozen || session->revoking || session->pending_operation ||
      session->wait_cancel.Token().IsCancelled())
    co_return Error{ErrorCode::kWouldBlock, "recovery_session_pending"};
  session->record = *record;
  session->frozen = true;
  auto rotated = co_await IssueClientRecoveryCredential(
      session, AccountSessionAction::kConsumeRecovery, request.proof);
  if (!rotated) {
    if (rotated.Error().Code() != ErrorCode::kCommitUnknown) session->frozen = false;
    co_return rotated.Error();
  }
  proxy = entity_mgr_.FindProxy(session->owner.entity_id);
  if (!proxy || proxy->IsPendingDestroy() || IsShuttingDown())
    co_return Error{ErrorCode::kCancelled, "recovery_entity_unavailable"};
  if (auto* previous = ResolveClientChannel(proxy->EntityId()))
    external_network_.DisconnectChannel(previous->RemoteAddress());
  UnbindClient(proxy->EntityId());
  ClearDetachedGrace(proxy->EntityId());
  if (!RotateProxySession(proxy->EntityId(), request.session_key))
    co_return Error{ErrorCode::kAlreadyExists, "recovery_session_key_conflict"};
  session->frozen = false;
  EnterDetachedGrace(proxy->EntityId());
  reply.success = true;
  reply.entity_id = proxy->EntityId();
  reply.external_addr = external_addr;
  co_return reply;
}

auto BaseApp::RecoverPersistentSession(login::PrepareResume request, AccountSession record)
    -> Task<Result<login::PrepareResumeResult>> {
  if (record.recovery.entity_dbid <= kInvalidDBID)
    co_return Error{ErrorCode::kPermissionDenied, "temporary_entity_cannot_recover_after_crash"};
  const auto self = std::ranges::find_if(baseapp_peers_, [this](const auto& entry) {
    return entry.second.process_instance == Config().process_instance;
  });
  if (self == baseapp_peers_.end() || self->second.external_addr.Port() == 0)
    co_return Error{ErrorCode::kReceiverGone, "recovery_public_address_unavailable"};
  const auto external_addr = self->second.external_addr;
  std::vector<EntityID> created;
  ScopeGuard rollback([this, &created] {
    for (const auto id : created) {
      auto* entity = entity_mgr_.Find(id);
      if (!entity) continue;
      ReleaseCheckout(entity->Dbid(), entity->TypeId(), entity->CheckoutOwner());
      (void)NotifyManagedEntityDestroyed(id, "recovery rollback");
      entity_mgr_.Destroy(id);
    }
  });
  const bool account_existed = entity_mgr_.FindByDbid(request.account_dbid) != nullptr;
  auto account = co_await LoadRecoveryEntity(request.account_dbid, request.account_type_id,
                                             record.recovery.entity_dbid == request.account_dbid);
  if (!account) co_return account.Error();
  if (account->status != dbapp::CheckoutStatus::kSuccess) {
    const auto holder = baseapp_peers_.find(account->holder_addr);
    if (account->status == dbapp::CheckoutStatus::kAlreadyCheckedOut &&
        holder != baseapp_peers_.end() &&
        holder->second.process_instance == account->holder_process_instance) {
      login::PrepareResumeResult reply;
      reply.redirect = holder->second.internal_addr;
      co_return reply;
    }
    co_return Error{ErrorCode::kPermissionDenied, "recovery_account_checkout_unavailable"};
  }
  const auto account_entity_id = account->holder_entity_id;
  if (!account_existed) created.push_back(account_entity_id);
  if (record.recovery.entity_dbid != request.account_dbid) {
    const bool target_existed = entity_mgr_.FindByDbid(record.recovery.entity_dbid) != nullptr;
    auto target = co_await LoadRecoveryEntity(record.recovery.entity_dbid,
                                              record.recovery.entity_type_id, true);
    if (!target) co_return target.Error();
    if (target->status != dbapp::CheckoutStatus::kSuccess)
      co_return Error{ErrorCode::kPermissionDenied, "recovery_target_checkout_unavailable"};
    if (!target_existed) created.push_back(target->holder_entity_id);
  }
  auto* account_entity = entity_mgr_.Find(account_entity_id);
  auto* target = entity_mgr_.FindByDbid(record.recovery.entity_dbid);
  if (!account_entity || !account_entity->CheckoutOwner() || !target ||
      !entity_mgr_.FindProxy(target->EntityId()) || !target->CheckoutOwner() || IsShuttingDown())
    co_return Error{ErrorCode::kPermissionDenied, "recovery_persistent_binding_unavailable"};
  auto session = std::make_shared<ClientSession>();
  session->account_dbid = request.account_dbid;
  session->account_type_id = request.account_type_id;
  session->owner = {target->CheckoutOwner()->base_addr, target->EntityId(),
                    Config().process_instance};
  session->record = record;
  session->supports_recovery = true;
  const auto entity_id = target->EntityId();
  if (!client_sessions_.emplace(entity_id, session).second)
    co_return Error{ErrorCode::kWouldBlock, "recovery_session_already_pending"};
  auto claimed =
      co_await IssueClientRecoveryCredential(session, AccountSessionAction::kClaimRecovery,
                                             request.proof, account_entity->CheckoutOwner());
  if (!claimed) {
    if (claimed.Error().Code() == ErrorCode::kCommitUnknown) {
      rollback.Dismiss();
    } else {
      client_sessions_.erase(entity_id);
    }
    co_return claimed.Error();
  }
  rollback.Dismiss();
  if (!RotateProxySession(entity_id, request.session_key))
    co_return Error{ErrorCode::kAlreadyExists, "recovery_session_key_conflict"};
  session->frozen = false;
  EnterDetachedGrace(entity_id);
  login::PrepareResumeResult reply;
  reply.success = true;
  reply.entity_id = entity_id;
  reply.external_addr = external_addr;
  co_return reply;
}

}  // namespace atlas
