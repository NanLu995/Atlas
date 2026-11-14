#include "dbapp.h"
#include "foundation/intrusive_ptr.h"
#include "network/channel.h"

namespace atlas {

void DBApp::OnAccountSession(const Address& src, Channel* ch,
                             const dbapp::AccountSessionRequest& msg) {
  if (ch == nullptr || ch->RemoteAddress() != src) return;
  auto respond = [msg, channel = IntrusivePtr<Channel>{ch}](Result<AccountSession> result) {
    dbapp::AccountSessionReply reply;
    reply.request_id = msg.request_id;
    reply.requester_instance = msg.process_instance;
    reply.account_dbid = msg.account_dbid;
    reply.account_type_id = msg.account_type_id;
    if (msg.operation) reply.operation_id = msg.operation->id;
    if (result) {
      reply.session = *result;
    } else {
      reply.status = result.Error().Code();
      reply.error = result.Error().Message();
      if (reply.error.size() > 512) reply.error.resize(512);
    }
    if (!channel->IsCondemned()) (void)channel->SendMessage(reply);
  };
  const auto known = baseapp_instances_.find(src);
  if (msg.process_instance == 0 || known == baseapp_instances_.end() ||
      known->second != msg.process_instance) {
    respond(Error{ErrorCode::kPermissionDenied, "unregistered BaseApp process instance"});
    return;
  }
  if (msg.request_id == 0 || msg.account_dbid <= kInvalidDBID || msg.account_type_id == 0) {
    respond(Error{ErrorCode::kInvalidArgument, "invalid account session request"});
    return;
  }
  if (msg.account_type_id != account_type_id_) {
    respond(Error{ErrorCode::kInvalidArgument,
                  "account session type does not match configured account"});
    return;
  }
  if (!database_) {
    respond(Error{ErrorCode::kReceiverGone, "account session database unavailable"});
    return;
  }
  if (!AcceptsShardTableVersion(msg.shard_table_version)) {
    respond(Error{ErrorCode::kInvalidArgument, std::string(dbapp::kInvalidShardTableError)});
    return;
  }
  if (HasAuthoritativeShardTable()) {
    const auto* shard = FindShard(msg.account_dbid);
    if (shard == nullptr || shard->dbapp_id != dbapp_id_) {
      respond(Error{ErrorCode::kPermissionDenied, "account session is outside this DBApp range"});
      return;
    }
  }
  auto op = msg.operation;
  if (op) {
    if (op->account_dbid != msg.account_dbid || op->account_type_id != msg.account_type_id ||
        op->actor.process_instance != msg.process_instance ||
        op->actor.base_addr.Port() != src.Port() ||
        (op->actor.base_addr.Ip() != 0 && op->actor.base_addr != src)) {
      respond(Error{ErrorCode::kPermissionDenied, "account session actor mismatch"});
      return;
    }
    op->actor.base_addr = src;
    if (auto valid = ValidateSessionOperation(*op); !valid) {
      respond(valid.Error());
      return;
    }
    if (op->action == AccountSessionAction::kPrepare ||
        op->action == AccountSessionAction::kMoveLocal) {
      const auto target = baseapp_instances_.find(op->target.base_addr);
      if (target == baseapp_instances_.end() || target->second != op->target.process_instance) {
        respond(Error{ErrorCode::kPermissionDenied, "unregistered target BaseApp instance"});
        return;
      }
    }
  }
  if (account_session_requests_ >= kMaxAccountSessionRequests) {
    respond(Error{ErrorCode::kRateLimited, "account session request queue full"});
    return;
  }
  ++account_session_requests_;
  auto complete = [this, respond = std::move(respond)](Result<AccountSession> value) {
    --account_session_requests_;
    respond(std::move(value));
  };
  if (op)
    database_->UpdateAccountSession(*op, std::move(complete));
  else
    database_->ReadAccountSession(msg.account_dbid, msg.account_type_id, std::move(complete));
}

}  // namespace atlas
