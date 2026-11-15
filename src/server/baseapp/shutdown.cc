#include "baseapp.h"
#include "foundation/log.h"

namespace atlas {

void BaseApp::BeginShutdown() {
  EntityApp::BeginShutdown();
  external_network_.StopAccepting();
  ScheduleShutdownSaves();
}

void BaseApp::ScheduleShutdownSaves() {
  entity_mgr_.FlushDestroyed();
  std::vector<EntityID> entities;
  entity_mgr_.ForEach([&](const BaseEntity& entity) {
    if (!entity.IsPendingDestroy() &&
        !prepared_login_requests_by_entity_.contains(entity.EntityId()) &&
        shutdown_save_started_.insert(entity.EntityId()).second) {
      shutdown_saves_.insert(entity.EntityId());
      entities.push_back(entity.EntityId());
    }
  });
  for (const auto entity_id : entities) StartDisconnectLogoff(entity_id);
}

void BaseApp::OnShutdownTick() {
  EntityApp::OnShutdownTick();
  CleanupExpiredPendingRequests();
  SweepIncomingClientHandoffs();
  SweepStaleDestroysInFlight();
  RetryCanceledCheckouts();
  RetryCheckins();
  RetryDisconnectedDbappLogoffWrites();
  RetryDisconnectedDbappWriteRequests();
  ScheduleShutdownSaves();
}

auto BaseApp::PendingShutdownOperations() const -> std::size_t {
  return EntityApp::PendingShutdownOperations() + external_network_.PendingSendCount() +
         rpc_registry_.PendingCount() + rpc_registry_.ReplyCount() + shutdown_saves_.size() +
         pending_checkins_.size() + pending_logoff_writes_.size() + pending_write_to_db_.size() +
         pending_entity_checkouts_.size() + canceled_checkouts_.size() + pending_logins_.size() +
         pending_force_logoffs_.size() + prepared_login_entities_.size() +
         login_session_checks_.size() + incoming_client_handoffs_.size() + session_revocations_ +
         destroy_ack_waiters_.size() + pending_space_creates_.size();
}

void BaseApp::LogShutdownPending() const {
  EntityApp::LogShutdownPending();
  for (const auto entity_id : shutdown_saves_)
    ATLAS_LOG_ERROR("BaseApp: shutdown save unconfirmed entity_id={}", entity_id);
  ATLAS_LOG_ERROR(
      "BaseApp: shutdown writes={} logoffs={} checkins={} checkouts={} rpc_waits={}",
      pending_write_to_db_.size(), pending_logoff_writes_.size(), pending_checkins_.size(),
      pending_entity_checkouts_.size() + canceled_checkouts_.size(), rpc_registry_.PendingCount());
}

void BaseApp::FinishShutdownSave(EntityID entity_id, bool success, std::string_view error) {
  if (shutdown_saves_.erase(entity_id) == 0) return;
  if (!success)
    MarkShutdownFailed(Error{ErrorCode::kIoError,
                             std::format("entity {} shutdown save failed: {}", entity_id, error)});
}

}  // namespace atlas
