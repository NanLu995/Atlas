#include "baseapp.h"
#include "entitydef/entity_def_registry.h"
#include "platform/secure_random.h"

namespace atlas {

auto BaseApp::StartRecoveryEntityLoad(
    DatabaseID dbid, uint16_t type_id, bool has_client,
    std::function<void(Result<dbapp::CheckoutEntityAck>)> complete) -> EntityID {
  if (IsShuttingDown() || pending_entity_checkouts_.size() >= 1024 ||
      !active_login_dbids_.insert(dbid).second) {
    complete(Error{ErrorCode::kWouldBlock, "recovery_entity_load_unavailable"});
    return kInvalidEntityID;
  }
  const auto* type = EntityDefs().FindById(type_id);
  auto* entity = type ? entity_mgr_.Create(type_id, has_client || type->has_client, dbid) : nullptr;
  if (!entity) {
    FinishLoginFlow(dbid);
    complete(Error{ErrorCode::kInvalidArgument, "recovery_entity_type_or_id_unavailable"});
    return kInvalidEntityID;
  }
  const auto entity_id = entity->EntityId();
  PendingDbCheckout pending;
  auto& request = pending.request;
  std::ranges::copy(EntityDefs().Digest(), request.entity_def_digest.begin());
  request.request_id = AllocatePrepareRequestId();
  request.owner_app_id = app_id_;
  request.owner_process_instance = Config().process_instance;
  request.type_id = type_id;
  request.dbid = dbid;
  request.entity_id = entity_id;
  pending.created_at = Clock::now();
  pending.resolved_dbid = dbid;
  if (!FillSecureRandom(request.operation_id) || request.request_id == 0 ||
      !SendEntityCheckout(pending)) {
    entity_mgr_.Destroy(entity_id);
    FinishLoginFlow(dbid);
    complete(Error{ErrorCode::kReceiverGone, "recovery_checkout_not_queued"});
    return kInvalidEntityID;
  }
  pending.complete = std::move(complete);
  pending_entity_checkouts_.emplace(request.request_id, std::move(pending));
  return entity_id;
}

auto BaseApp::LoadRecoveryEntity(DatabaseID dbid, uint16_t type_id, bool has_client)
    -> Task<Result<dbapp::CheckoutEntityAck>> {
  if (auto* entity = entity_mgr_.FindByDbid(dbid)) {
    const auto& checkout = entity->CheckoutOwner();
    const auto* proxy = entity_mgr_.FindProxy(entity->EntityId());
    if (active_login_dbids_.contains(dbid) || entity->IsPendingDestroy() ||
        entity->TypeId() != type_id || !checkout || checkout->ownership_version == 0 ||
        checkout->process_instance != Config().process_instance ||
        client_sessions_.contains(entity->EntityId()) || (proxy && proxy->HasClient()))
      co_return Error{ErrorCode::kPermissionDenied, "recovery_entity_already_in_use"};
    dbapp::CheckoutEntityAck result;
    result.dbid = dbid;
    result.checkout_version = checkout->ownership_version;
    result.holder_addr = checkout->base_addr;
    result.holder_app_id = checkout->app_id;
    result.holder_entity_id = entity->EntityId();
    result.holder_process_instance = checkout->process_instance;
    co_return result;
  }
  struct State {
    Result<dbapp::CheckoutEntityAck> result{Error{ErrorCode::kInternalError}};
    std::coroutine_handle<> continuation;
    EntityID entity_id{kInvalidEntityID};
    bool completed{false};
    bool suspending{true};

    void Complete(Result<dbapp::CheckoutEntityAck> value) {
      if (completed) return;
      completed = true;
      result = std::move(value);
      if (!suspending && continuation) continuation.resume();
    }
  };
  struct Awaiter {
    BaseApp& app;
    DatabaseID dbid;
    uint16_t type_id;
    bool has_client;
    std::shared_ptr<State> state{std::make_shared<State>()};
    CancelRegistration cancellation{};

    ~Awaiter() {
      state->continuation = {};
      if (!state->completed) app.entity_mgr_.Destroy(state->entity_id);
    }
    auto await_ready() const -> bool { return false; }
    auto await_suspend(std::coroutine_handle<> caller) -> bool {
      state->continuation = caller;
      cancellation = app.recovery_cancel_.Token().OnCancel([this] {
        if (state->completed) return;
        app.entity_mgr_.Destroy(state->entity_id);
        state->Complete(Error{ErrorCode::kCancelled, "recovery_load_cancelled"});
      });
      if (!state->completed)
        state->entity_id = app.StartRecoveryEntityLoad(
            dbid, type_id, has_client,
            [state = state](auto result) { state->Complete(std::move(result)); });
      state->suspending = false;
      return !state->completed;
    }
    auto await_resume() -> Result<dbapp::CheckoutEntityAck> { return std::move(state->result); }
  };
  co_return co_await Awaiter{*this, dbid, type_id, has_client};
}

}  // namespace atlas
