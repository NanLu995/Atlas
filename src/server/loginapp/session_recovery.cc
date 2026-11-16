#include "coro/rpc_call.h"
#include "coro/scope_guard.h"
#include "loginapp.h"
#include "network/channel.h"
#include "network/reliable_udp.h"
#include "platform/sha256.h"

namespace atlas {

void LoginApp::OnResumeRequest(const Address& src, Channel* channel,
                               const login::ResumeRequest& request) {
  if (!channel) return;
  const auto id = channel->ChannelId();
  if (IsShuttingDown() || !baseappmgr_channel_) {
    SendLoginError(id, login::LoginStatus::kServerNotReady, "recovery_unavailable");
    return;
  }
  if (!request.credential.IsPresent() || request.credential.account_dbid <= kInvalidDBID ||
      request.credential.account_type_id == 0) {
    SendLoginError(id, login::LoginStatus::kInvalidCredentials, "invalid_recovery_credential");
    return;
  }
  ++login_requests_total_;
  if (IsRateLimited(src)) {
    ++login_rate_limited_total_;
    SendLoginError(id, login::LoginStatus::kRateLimited, "too_many_attempts");
    return;
  }
  RecordLoginAttempt(src);
  if (channel_cancel_sources_.contains(id)) {
    ++login_dedup_total_;
    SendLoginError(id, login::LoginStatus::kLoginInProgress, "login_in_progress");
    return;
  }
  if (pending_by_username_.size() + pending_recoveries_.size() >= kMaxPendingLogins) {
    ++login_busy_total_;
    SendLoginError(id, login::LoginStatus::kServerBusy, "server_busy");
    return;
  }
  HandleResumeCoro(id, request);
}

auto LoginApp::HandleResumeCoro(uint64_t client_channel_id, login::ResumeRequest request)
    -> FireAndForget {
  const auto started = Clock::now();
  const auto key = std::pair{request.credential.account_dbid, request.credential.account_type_id};
  if (pending_recoveries_.contains(key)) {
    SendLoginError(client_channel_id, login::LoginStatus::kLoginInProgress, "login_in_progress");
    co_return;
  }
  const auto id = rpc_registry_.AllocateRequestId();
  if (id == 0) {
    SendLoginError(client_channel_id, login::LoginStatus::kServerBusy, "request_ids_unavailable");
    co_return;
  }
  pending_recoveries_.emplace(key, id);
  ScopeGuard pending_guard([this, key] { pending_recoveries_.erase(key); });
  CancellationSource cancel;
  auto token = cancel.Token();
  channel_cancel_sources_.emplace(client_channel_id, cancel);
  ScopeGuard cancel_guard([this, client_channel_id] {
    channel_cancel_sources_.erase(client_channel_id);
    if (auto* channel = external_network_.FindChannel(client_channel_id))
      channel->SetInactivityTimeout(std::chrono::seconds(kClientChannelInactivitySec));
  });
  if (auto* channel = external_network_.FindChannel(client_channel_id))
    channel->SetInactivityTimeout(Duration::zero());

  login::AllocateBaseApp allocation;
  allocation.request_id = next_request_id_++;
  allocation.type_id = request.credential.account_type_id;
  allocation.dbid = request.credential.account_dbid;
  IntrusivePtr<Channel> manager{baseappmgr_channel_};
  auto allocated = co_await RpcCall<login::AllocateBaseAppResult>(rpc_registry_, *manager,
                                                                  allocation, kRpcTimeout, token);
  if (!allocated || !allocated->success) {
    if (!token.IsCancelled())
      SendLoginError(client_channel_id, login::LoginStatus::kServerNotReady,
                     "recovery_baseapp_unavailable");
    co_return;
  }
  auto session_key = SessionKey::Generate();
  auto proof = Sha256(request.credential.value);
  if (!session_key || !proof) {
    SendLoginError(client_channel_id, login::LoginStatus::kInternalError, "recovery_key_failed");
    co_return;
  }
  login::PrepareResume prepare;
  prepare.request_id = id;
  prepare.requester_instance = Config().process_instance;
  prepare.account_dbid = request.credential.account_dbid;
  prepare.account_type_id = request.credential.account_type_id;
  prepare.proof = *proof;
  prepare.session_key = *session_key;
  prepare.entity_def_digest = request.entity_def_digest;
  auto target = allocated->internal_addr;
  for (int hop = 0; hop < 2; ++hop) {
    auto connection = Network().ConnectRudpNocwnd(target);
    if (!connection) break;
    IntrusivePtr<Channel> channel{*connection};
    auto prepared = co_await RpcCall<login::PrepareResumeResult>(rpc_registry_, *channel, prepare,
                                                                 kRpcTimeout, token);
    if (token.IsCancelled()) {
      ++abandoned_login_total_;
      co_return;
    }
    if (!prepared || prepared->requester_instance != prepare.requester_instance) break;
    if (prepared->success) {
      auto* client = external_network_.FindChannel(client_channel_id);
      if (!client) co_return;
      login::LoginResult reply;
      reply.status = login::LoginStatus::kSuccess;
      reply.session_key = *session_key;
      reply.baseapp_addr = prepared->external_addr;
      if (client->SendMessage(reply)) {
        ++login_success_total_;
        login_latency_.Record(Clock::now() - started);
      } else {
        ++abandoned_login_total_;
      }
      co_return;
    }
    if (hop == 0 && prepared->redirect.Port() != 0 && prepared->redirect != target) {
      target = prepared->redirect;
      continue;
    }
    SendLoginError(client_channel_id, login::LoginStatus::kInvalidCredentials,
                   prepared->error.empty() ? "recovery_rejected" : prepared->error);
    co_return;
  }
  SendLoginError(client_channel_id, login::LoginStatus::kServerNotReady,
                 "recovery_result_unconfirmed");
}

}  // namespace atlas
