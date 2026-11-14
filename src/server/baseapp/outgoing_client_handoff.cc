#include "baseapp.h"
#include "coro/rpc_call.h"
#include "platform/secure_random.h"
#include "platform/sha256.h"

namespace atlas {
auto BaseApp::CallClientHandoff(Channel& channel, baseapp::PrepareClientHandoff request,
                                CancellationToken token) -> Task<Result<AccountSession>> {
  request.request_id = rpc_registry_.AllocateRequestId();
  if (request.request_id == 0)
    co_return Error{ErrorCode::kOutOfRange, "client handoff request IDs exhausted"};
  IntrusivePtr<Channel> retained{&channel};
  auto reply = co_await RpcCall<baseapp::PrepareClientHandoffReply>(
      rpc_registry_, *retained, request,
      request.wait_for_client ? std::chrono::seconds(15) : std::chrono::seconds(5), token);
  if (!reply) co_return reply.Error();
  if (reply->requester_instance != Config().process_instance ||
      reply->account_dbid != request.account_dbid ||
      reply->account_type_id != request.account_type_id)
    co_return Error{ErrorCode::kCommitUnknown, "client handoff reply correlation mismatch"};
  if (reply->status != ErrorCode::kNone) co_return Error{reply->status, reply->error};
  co_return reply->session;
}

auto BaseApp::RecoverClientHandoff(std::shared_ptr<ClientSession> session,
                                   std::array<std::byte, 16> handoff_id, Error error)
    -> Task<Result<void>> {
  const auto entity_id = session->owner.entity_id;
  const auto found = client_sessions_.find(entity_id);
  if (found == client_sessions_.end() || found->second != session || session->revoking ||
      session->wait_cancel.Token().IsCancelled())
    co_return Error{ErrorCode::kCommitUnknown, "client handoff source changed"};
  if (session->pending_operation) {
    auto resolved = co_await ApplyClientSessionOperation(session, *session->pending_operation,
                                                         session->wait_cancel.Token());
    if (!resolved && resolved.Error().Code() == ErrorCode::kCommitUnknown)
      co_return resolved.Error();
  }
  auto record = co_await QueryAccountSession(session->account_dbid, session->account_type_id, {},
                                             session->wait_cancel.Token());
  if (record && record->phase >= AccountSessionPhase::kPreparing &&
      record->owner == session->owner && record->handoff_id == handoff_id) {
    auto abort = MakeSessionOperation(session->account_dbid, session->account_type_id, *record,
                                      session->owner, AccountSessionAction::kAbort);
    if (abort)
      record = co_await ApplyClientSessionOperation(session, *abort, session->wait_cancel.Token());
    else
      record = abort.Error();
  }
  auto current = client_sessions_.find(entity_id);
  const auto* proxy = entity_mgr_.FindProxy(entity_id);
  if (!record || record->phase != AccountSessionPhase::kActive || record->owner != session->owner ||
      session->pending_operation || session->wait_cancel.Token().IsCancelled() ||
      current == client_sessions_.end() || current->second != session || !proxy ||
      proxy->IsPendingDestroy() || !proxy->HasClient() || proxy->GetSessionKey().IsZero())
    co_return Error{ErrorCode::kCommitUnknown, "client handoff outcome was not confirmed"};
  session->record = *record;
  session->frozen = false;
  session->remote_handoff = false;
  auto* channel = ResolveClientChannel(entity_id);
  if (channel == nullptr || channel->IsCondemned()) {
    UnbindClient(entity_id);
    if (proxy && proxy->Dbid() != kInvalidDBID)
      EnterDetachedGrace(entity_id);
    else
      StartDisconnectLogoff(entity_id);
  }
  co_return error;
}

auto BaseApp::MoveRemoteClientSession(EntityID source, AccountSessionOwner target)
    -> Task<Result<void>> {
  const auto found = client_sessions_.find(source);
  auto* proxy = entity_mgr_.FindProxy(source);
  const auto peer = baseapp_peers_.find(target.base_addr);
  const auto username = account_index_reverse_.find(source);
  if (!IsValidSessionOwner(target) || target.process_instance == Config().process_instance ||
      peer == baseapp_peers_.end() || peer->second.process_instance != target.process_instance ||
      peer->second.external_addr.Ip() == 0 || peer->second.external_addr.Port() == 0 || !proxy ||
      proxy->IsPendingDestroy() || !proxy->HasClient() || proxy->GetSessionKey().IsZero() ||
      found == client_sessions_.end() || found->second->frozen || found->second->busy ||
      found->second->pending_operation || found->second->revoking ||
      username == account_index_reverse_.end())
    co_return Error{ErrorCode::kInvalidArgument,
                    "client handoff source or destination unavailable"};
  const auto client_address = peer->second.external_addr;
  const auto account_name = username->second;
  auto session = found->second;
  if (!session->supports_handoff)
    co_return Error{ErrorCode::kNotSupported, "client does not support BaseApp handoff"};
  if (session->record.phase != AccountSessionPhase::kActive ||
      session->record.owner != session->owner)
    co_return Error{ErrorCode::kPermissionDenied,
                    "client handoff source does not hold the session"};
  auto* route = ResolveInternalChannel(target.base_addr);
  if (route == nullptr || route->IsCondemned())
    co_return Error{ErrorCode::kReceiverGone, "client handoff target channel unavailable"};
  IntrusivePtr<Channel> channel{route};
  auto credential = SessionKey::Generate();
  if (!credential) co_return credential.Error();
  if (credential->IsZero())
    co_return Error{ErrorCode::kInternalError, "client handoff credential unavailable"};
  auto hash = Sha256(std::as_bytes(std::span{credential->bytes}));
  if (!hash) co_return hash.Error();
  auto prepare =
      MakeSessionOperation(session->account_dbid, session->account_type_id, session->record,
                           session->owner, AccountSessionAction::kPrepare);
  if (!prepare) co_return prepare.Error();
  if (auto random = FillSecureRandom(prepare->handoff_id); !random) co_return random.Error();
  prepare->target = target;
  prepare->credential_hash = *hash;
  const auto handoff_id = prepare->handoff_id;
  session->frozen = true;
  session->remote_handoff = true;
  auto prepared =
      co_await ApplyClientSessionOperation(session, *prepare, session->wait_cancel.Token());
  if (!prepared) co_return co_await RecoverClientHandoff(session, handoff_id, prepared.Error());
  baseapp::PrepareClientHandoff request;
  request.requester_instance = Config().process_instance;
  request.account_dbid = session->account_dbid;
  request.account_type_id = session->account_type_id;
  request.expected = *prepared;
  request.username = account_name;
  std::ranges::copy(std::as_bytes(EntityDefRegistry::Instance().Digest()),
                    request.entity_def_digest.begin());
  auto accepted = co_await CallClientHandoff(*channel, request, session->wait_cancel.Token());
  if (!accepted) co_return co_await RecoverClientHandoff(session, handoff_id, accepted.Error());
  auto expected = *prepared;
  expected.phase = AccountSessionPhase::kAccepted;
  ++expected.version;
  if (*accepted != expected)
    co_return co_await RecoverClientHandoff(
        session, handoff_id,
        Error{ErrorCode::kCommitUnknown, "client handoff acceptance mismatch"});
  auto await_client =
      MakeSessionOperation(session->account_dbid, session->account_type_id, *accepted,
                           session->owner, AccountSessionAction::kAwaitClient);
  if (!await_client)
    co_return co_await RecoverClientHandoff(session, handoff_id, await_client.Error());
  auto awaiting =
      co_await ApplyClientSessionOperation(session, *await_client, session->wait_cancel.Token());
  if (!awaiting) co_return co_await RecoverClientHandoff(session, handoff_id, awaiting.Error());
  auto* client = ResolveClientChannel(source);
  if (!client || client->IsCondemned() || session->wait_cancel.Token().IsCancelled())
    co_return co_await RecoverClientHandoff(
        session, handoff_id, Error{ErrorCode::kReceiverGone, "client handoff client disconnected"});
  baseapp::ClientHandoff redirect;
  redirect.target = target;
  redirect.target.base_addr = client_address;
  redirect.credential = *credential;
  if (auto sent = client->SendMessage(redirect); !sent)
    co_return co_await RecoverClientHandoff(session, handoff_id, sent.Error());
  request.wait_for_client = true;
  auto confirmed = co_await CallClientHandoff(*channel, request, session->wait_cancel.Token());
  if (!confirmed) co_return co_await RecoverClientHandoff(session, handoff_id, confirmed.Error());
  expected = *awaiting;
  expected.phase = AccountSessionPhase::kActive;
  expected.owner = target;
  expected.target = {};
  expected.credential_hash = {};
  ++expected.version;
  const auto current = client_sessions_.find(source);
  if (*confirmed != expected || current == client_sessions_.end() || current->second != session ||
      session->wait_cancel.Token().IsCancelled())
    co_return Error{ErrorCode::kCommitUnknown, "client handoff final confirmation mismatch"};
  (void)entity_mgr_.ClearSessionKey(source);
  UnbindClient(source);
  UnregisterAccountOwner(source);
  client_sessions_.erase(current);
  co_return Result<void>{};
}

}  // namespace atlas
