#ifndef ATLAS_SERVER_BASEAPP_BASEAPP_H_
#define ATLAS_SERVER_BASEAPP_BASEAPP_H_

#include <functional>
#include <memory>
#include <span>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "baseapp_messages.h"
#include "baseapp_native_provider.h"
#include "cellapp/cellapp_messages.h"
#include "client_handoff_messages.h"
#include "coro/cancellation.h"
#include "coro/fire_and_forget.h"
#include "coro/pending_rpc_registry.h"
#include "coro/task.h"
#include "db/idatabase.h"
#include "dbapp/account_session_messages.h"
#include "dbapp/dbapp_messages.h"
#include "dbappmgr/dbappmgr_messages.h"
#include "delta_forwarder.h"
#include "entity_manager.h"
#include "foundation/clock.h"
#include "foundation/intrusive_ptr.h"
#include "foundation/latency_histogram.h"
#include "math/vector3.h"
#include "network/channel.h"
#include "server/cellapp_peer_registry.h"
#include "server/entity_app.h"
#include "server/entity_types.h"
#include "server/id_client.h"
#include "session_messages.h"

namespace atlas {

namespace baseapp {
struct CreateBase;
struct CreateBaseFromDB;
struct CellEntityCreated;
struct CellEntityCreateFailed;
struct CellEntityDestroyed;
struct CurrentCell;
struct CellRpcForward;
struct BroadcastRpcFromCell;
struct ReplicatedDeltaFromCell;
struct ReplicatedReliableDeltaFromCell;
struct BackupCellEntity;
struct ReplicatedBaselineFromCell;
struct ForceLogoff;
struct ForceLogoffAck;
struct CellAppDeath;
struct ClientEventSeqReport;
struct Authenticate;
struct ClientBaseRpc;
struct ClientCellRpc;
struct ClientMovementInput;
struct MovementCorrectionReport;
struct MovementCommandStartFromCell;
struct MovementCommandEndFromCell;
struct MovementStateAckFromCell;
}  // namespace baseapp

namespace login {
struct PrepareLogin;
struct PrepareLoginResult;
struct CancelPrepareLogin;
}  // namespace login

namespace baseappmgr {
struct InformLoad;
struct RegisterBaseAppAck;
}  // namespace baseappmgr

namespace cellappmgr {
struct SpaceCreatedResult;
}  // namespace cellappmgr

namespace cellapp {
struct DestroyCellEntityAck;
}  // namespace cellapp

class Channel;

// Entity-bearing server process: hosts BaseEntity/Proxy, dual networks,
// DB persistence, and scripted entity logic.

// Returns nullptr if cell_addr has port 0 or no entry exists.
// Free function so unit tests can drive it without a BaseApp.
[[nodiscard]] auto ResolveCellChannelByAddr(
    const std::unordered_map<Address, IntrusivePtr<Channel>>& cellapp_channels,
    const Address& cell_addr) -> Channel*;

class BaseApp : public EntityApp {
 public:
  static auto Run(int argc, char* argv[]) -> int;

  BaseApp(EventDispatcher& dispatcher, NetworkInterface& internal_network,
          NetworkInterface& external_network);
  ~BaseApp() override;

  [[nodiscard]] auto GetEntityManager() -> EntityManager& { return entity_mgr_; }
  [[nodiscard]] auto GetNativeProvider() -> BaseAppNativeProvider& { return *native_provider_; }

  // Shared (reply_id, request_id) registry backs both C++ coroutines and
  // managed AtlasTask RPCs via coro_bridge.
  [[nodiscard]] auto GetRpcRegistry() -> PendingRpcRegistry& { return rpc_registry_; }

  // CellApp channel currently owning target_entity_id's Real; nullptr if
  // unknown, no cell yet, or peer map missing entry.
  [[nodiscard]] auto ResolveCellChannelForEntity(EntityID target_entity_id) const -> Channel*;

  using SpaceCreatedCallback =
      std::function<void(bool success, SpaceID space_id, const Address& cell_addr)>;

  // Returns 0 if no CellAppMgr connected; else request_id.
  // initial_cell_count > 1 asks CellAppMgr to bootstrap distributed cells.
  auto RequestCreateSpace(SpaceID space_id, SpaceCreatedCallback callback,
                          uint16_t initial_cell_count = 1, bool reuse_existing = false) -> uint32_t;

  // Empty type_name unregisters. Registration also eagerly fires the
  // CreateSpaceRequest so the space master is alive before any client login.
  void SetSpaceMasterType(SpaceID space_id, std::string type_name);

 protected:
  void BeginShutdown() override;
  void OnShutdownTick() override;
  [[nodiscard]] auto PendingShutdownOperations() const -> std::size_t override;
  void LogShutdownPending() const override;
  [[nodiscard]] auto Init(int argc, char* argv[]) -> bool override;
  void Fini() override;

  void OnEndOfTick() override;
  void OnTickComplete() override;
  void FlushTickDirtyChannels() override;
  void RegisterWatchers() override;

  [[nodiscard]] auto CreateNativeProvider() -> std::unique_ptr<INativeApiProvider> override;

 private:
  void ScheduleShutdownSaves();
  void FinishShutdownSave(EntityID entity_id, bool success, std::string_view error);
  std::unordered_set<EntityID> shutdown_save_started_;
  std::unordered_set<EntityID> shutdown_saves_;
  friend class BaseAppRollbackTest;
  friend class BaseAppMovementInputTest;
  friend class BaseAppInflightRpcTest;

  struct LoadSnapshot;
  struct LoadTracker;

  void QueuePendingAoIRadius(EntityID entity_id, float radius, float hysteresis);

  // Dispatch via mgr-bootstrapped Space when CellAppMgr is connected, with a
  // legacy direct-peer fallback for unit-test setups that have no mgr.
  void DispatchCreateCellEntity(cellapp::CreateCellEntity msg);
  void DispatchSpawnLocalEntity(cellapp::SpawnLocalEntity msg);
  void EnsureSpaceBootstrap(SpaceID space_id);
  void FlushPendingCellEntities(SpaceID space_id, const Address& host);
  void FlushPendingSpawnLocalEntities(SpaceID space_id, const Address& host);
  void OnSpaceHostKnown(SpaceID space_id, const Address& host);
  void OnSpaceBootstrapFailed(SpaceID space_id);

  void OnCreateBase(Channel& ch, const baseapp::CreateBase& msg);
  void OnCreateBaseFromDb(Channel& ch, const baseapp::CreateBaseFromDB& msg);
  void OnCellEntityCreated(Channel& ch, const baseapp::CellEntityCreated& msg);
  void OnCellEntityCreateFailed(const baseapp::CellEntityCreateFailed& msg);
  void OnCellEntityDestroyed(Channel& ch, const baseapp::CellEntityDestroyed& msg);
  void OnCurrentCell(Channel& ch, const baseapp::CurrentCell& msg);
  // Re-ships locally-tracked Reals on the dead addr to the rehome leaf,
  // seeded with the last cached cell backup and pose.
  void OnCellAppDeath(const baseapp::CellAppDeath& msg);
  void OnSpaceBspGeometry(const baseapp::SpaceBspGeometry& msg);
  void OnCellRpcForward(Channel& ch, const baseapp::CellRpcForward& msg);
  void OnBroadcastRpcFromCell(Channel& ch, const baseapp::BroadcastRpcFromCell& msg);
  // Wraps target_entity_id + rpc_id + trace_id + payload in kClientRpcMessageId envelope.
  RpcSendResult RelayRpcToClient(Channel& client_ch, EntityID target_entity_id, uint32_t rpc_id,
                                 const std::vector<std::byte>& payload, uint64_t trace_id);
  void OnReplicatedDeltaFromCell(Channel& ch, const baseapp::ReplicatedDeltaFromCell& msg);
  void OnReplicatedReliableDeltaFromCell(Channel& ch,
                                         const baseapp::ReplicatedReliableDeltaFromCell& msg);
  void OnBackupCellEntity(const baseapp::BackupCellEntity& msg);
  void OnReplicatedBaselineFromCell(const baseapp::ReplicatedBaselineFromCell& msg);
  void OnMovementStateAckFromCell(Channel& ch, const baseapp::MovementStateAckFromCell& msg);
  void OnMovementCommandStartFromCell(Channel& ch,
                                      const baseapp::MovementCommandStartFromCell& msg);
  void OnMovementCommandEndFromCell(Channel& ch,
                                    const baseapp::MovementCommandEndFromCell& msg);
  void SweepMovementAckRelayState();
  void OnSpaceCreatedResult(Channel& ch, const cellappmgr::SpaceCreatedResult& msg);

  void OnPrepareLogin(Channel& ch, const login::PrepareLogin& msg);
  void OnCancelPrepareLogin(Channel& ch, const login::CancelPrepareLogin& msg);
  void OnForceLogoff(Channel& ch, const baseapp::ForceLogoff& msg);
  void OnForceLogoffAck(Channel& ch, const baseapp::ForceLogoffAck& msg);
  void OnClientEventSeqReport(const baseapp::ClientEventSeqReport& msg);
  void OnRegisterBaseappAck(Channel& ch, const baseappmgr::RegisterBaseAppAck& msg);
  void OnGetEntityIdsAck(Channel& ch, const dbapp::GetEntityIdsAck& msg);

  void OnClientAuthenticate(Channel& ch, const baseapp::Authenticate& msg);
  void FinishClientAuthenticate(Channel& ch, const baseapp::Authenticate& msg);
  void OnClientBaseRpc(Channel& ch, const baseapp::ClientBaseRpc& msg);
  void OnClientCellRpc(Channel& ch, const baseapp::ClientCellRpc& msg);
  void ForwardClientCellRpc(Channel& client, Channel& cell, EntityID source_entity_id,
                            const baseapp::ClientCellRpc& msg);
  void OnClientMovementInput(Channel& ch, const baseapp::ClientMovementInput& msg);
  void OnMovementCorrectionReport(Channel& ch, const baseapp::MovementCorrectionReport& msg);

  friend class BaseAppNativeProvider;
  auto DoWriteToDb(EntityID entity_id, const std::byte* data, int32_t len,
                   uint64_t reply_request_id = 0) -> RpcSendResult;
  auto DoGiveClientToLocal(EntityID src_id, EntityID dest_id) -> Result<void>;
  auto NotifyClientCellReady(EntityID entity_id) -> Result<void>;

  // Allocates an ID, hydrates C#, and sends CreateCellEntity if cell-bound.
  // Returns the new EntityID, or 0 on failure.
  auto CreateBaseEntityFromScript(uint16_t type_id, SpaceID space_id, math::Vector3 position,
                                  math::Vector3 direction, bool on_ground) -> EntityID;

  // Routes SpawnLocalEntity to a deterministically-picked CellApp; the cell
  // assigns the id, so this returns only whether the message went out.
  auto RequestSpawnCellOnly(uint16_t type_id, SpaceID space_id, math::Vector3 position,
                            math::Vector3 direction, bool on_ground) -> bool;

  void FlushClientDeltas();

  // Reliable full-state snapshot per client-bound entity every
  // kBaselineInterval ticks; refreshes owner-scope state after resync gaps.
  void EmitBaselineSnapshots();

  void RegisterInternalHandlers();
  void ExpireDetachedProxies();
  void UpdateLoadEstimate();
  void ReportLoadToBaseAppMgr();
  [[nodiscard]] auto CaptureLoadSnapshot() const -> LoadSnapshot;
  void DrainFinishedLoginFlows(std::vector<DatabaseID> dbids);
  void MaybeRequestMoreIds();
  void OnDbAppBirth(const machined::BirthNotification& msg);
  void OnDbAppDeath(const machined::DeathNotification& msg);
  void OnDbAppMgrBirth(const machined::BirthNotification& msg);
  void OnDbAppMgrDeath(const machined::DeathNotification& msg);
  void OnShardTableResponse(const dbappmgr::ShardTableResponse& msg);
  void OnShardTableUpdate(const dbappmgr::ShardTableUpdate& msg);
  void RequestDbAppShardTable();
  void ApplyDbAppShardTable(uint32_t version, std::vector<dbappmgr::ShardEntry> entries);
  struct PendingLogin;
  struct PendingLogoffWrite;
  struct PendingWriteToDb;
  void MarkDbappLoginCheckoutsDisconnected(const Address& dbapp_addr);
  void MarkDbappLogoffWritesDisconnected(const Address& dbapp_addr);
  void MarkDbappWriteRequestsDisconnected(const Address& dbapp_addr);
  void RetryDisconnectedDbappLoginCheckouts();
  void RetryDisconnectedDbappLogoffWrites();
  void RetryDisconnectedDbappWriteRequests();
  [[nodiscard]] auto SendPendingLoginCheckout(uint32_t request_id, PendingLogin& pending,
                                              std::string_view* failure_reason = nullptr) -> bool;
  [[nodiscard]] auto SendPendingLogoffWriteToDb(uint32_t request_id, PendingLogoffWrite& pending)
      -> bool;
  [[nodiscard]] auto SendPendingWriteToDb(uint32_t request_id, PendingWriteToDb& pending) -> bool;
  void CompletePersistenceWait(uint64_t request_id, DatabaseID dbid, int32_t error_code,
                               std::string_view error);
  [[nodiscard]] auto ResolveDbAppChannel(DatabaseID dbid) -> Channel*;
  [[nodiscard]] auto ConnectDbAppChannel(const Address& addr) -> Channel*;
  [[nodiscard]] auto FindDbAppShard(DatabaseID dbid) const -> const dbappmgr::ShardEntry*;
  auto QueryAccountSession(DatabaseID dbid, uint16_t type_id,
                           std::optional<AccountSessionOperation> operation,
                           CancellationToken token = {}) -> Task<Result<AccountSession>>;
  void OnAccountSessionReply(Channel* channel, const dbapp::AccountSessionReply& reply);
  static auto MakeSessionOperation(DatabaseID dbid, uint16_t type_id, const AccountSession& record,
                                   const AccountSessionOwner& owner, AccountSessionAction action)
      -> Result<AccountSessionOperation>;
  struct ClientSession {
    DatabaseID account_dbid{kInvalidDBID};
    uint16_t account_type_id{0};
    uint32_t prepared_request_id{0};
    EntityID transfer_target{kInvalidEntityID};
    AccountSessionOwner owner;
    AccountSession record;
    std::optional<AccountSessionOperation> pending_operation;
    CancellationSource wait_cancel;
    bool busy{false};
    bool frozen{true};
    bool revoking{false};
    bool remote_handoff{false};
    bool supports_handoff{false};
    AccountSessionOwner handoff_source;
    uint64_t handoff_prepared_version{0};
  };
  auto AuthenticateClientSession(IntrusivePtr<Channel> channel, baseapp::Authenticate request,
                                 std::shared_ptr<ClientSession> session) -> FireAndForget;
  auto ReloginAfterSessionRelease(EntityID entity_id, PendingLogin pending) -> FireAndForget;
  auto ApplyClientSessionOperation(std::shared_ptr<ClientSession> session,
                                   AccountSessionOperation operation, CancellationToken token = {})
      -> Task<Result<AccountSession>>;
  auto ReleaseClientSession(EntityID entity_id, std::optional<uint64_t> expected_version = {})
      -> Task<Result<void>>;
  auto ReleaseClientSessionThen(EntityID entity_id, std::function<void(bool)> complete)
      -> FireAndForget;
  auto MoveClientSession(EntityID source, EntityID destination) -> Task<Result<void>>;
  auto MoveRemoteClientSession(EntityID source, AccountSessionOwner target) -> Task<Result<void>>;
  auto RecoverClientHandoff(std::shared_ptr<ClientSession> session,
                            std::array<std::byte, 16> handoff_id, Error error)
      -> Task<Result<void>>;
  auto CallClientHandoff(Channel& channel, baseapp::PrepareClientHandoff request,
                         CancellationToken token) -> Task<Result<AccountSession>>;
  auto StartClientTransfer(EntityID source, EntityID destination, uint64_t request_id,
                           AccountSessionOwner target = {}) -> RpcSendResult;
  auto CompleteClientTransfer(EntityID source, EntityID destination, uint64_t request_id,
                              intptr_t reply, AccountSessionOwner target) -> FireAndForget;
  [[nodiscard]] auto HasFrozenClientSession(EntityID entity_id) const -> bool;
  std::unordered_map<EntityID, std::shared_ptr<ClientSession>> client_sessions_;
  std::unordered_set<EntityID> client_transfer_targets_;
  struct IncomingClientHandoff {
    baseapp::PrepareClientHandoff request;
    std::shared_ptr<ClientSession> session;
    SessionKey credential_hash;
    TimePoint checked_at{Clock::now()};
    bool authenticating{false};
    bool expiring{false};
    struct Waiter {
      IntrusivePtr<Channel> channel;
      baseapp::PrepareClientHandoff request;
    };
    std::optional<Waiter> waiter;
  };
  void RegisterClientHandoffHandlers();
  auto OnPrepareClientHandoff(IntrusivePtr<Channel> channel, baseapp::PrepareClientHandoff request)
      -> FireAndForget;
  void OnAwaitClientHandoff(IntrusivePtr<Channel> channel,
                            const baseapp::PrepareClientHandoff& request);
  auto TryAuthenticateClientHandoff(Channel& channel, const baseapp::Authenticate& request) -> bool;
  auto AuthenticateClientHandoff(IntrusivePtr<Channel> channel, baseapp::Authenticate request,
                                 std::shared_ptr<IncomingClientHandoff> incoming) -> FireAndForget;
  void DropIncomingClientHandoff(EntityID entity_id);
  auto IsCurrentIncomingClientHandoff(const IncomingClientHandoff& incoming) const -> bool;
  void SweepIncomingClientHandoffs();
  auto ExpireIncomingClientHandoff(std::shared_ptr<IncomingClientHandoff> incoming)
      -> FireAndForget;
  std::unordered_map<EntityID, std::shared_ptr<IncomingClientHandoff>> incoming_client_handoffs_;
  std::unordered_map<SessionKey, EntityID> incoming_handoff_credentials_;
  void OnBaseAppBirth(const machined::BirthNotification& notification);
  void OnBaseAppDeath(const machined::DeathNotification& notification);
  auto OnRevokeAccountSession(IntrusivePtr<Channel> channel, baseapp::RevokeAccountSession request)
      -> FireAndForget;
  void OnRevokeAccountSessionReply(Channel* channel,
                                   const baseapp::RevokeAccountSessionReply& reply);
  auto RevokeSessionOwner(DatabaseID dbid, uint16_t type_id, AccountSession record,
                          CancellationToken token) -> Task<Result<void>>;
  auto CheckSessionBeforeLogin(PendingLogin pending, uint64_t ticket) -> FireAndForget;
  std::unordered_map<Address, machined::BirthNotification> baseapp_peers_;
  std::size_t session_revocations_{0};

  NetworkInterface& external_network_;
  IDClient id_client_;
  EntityManager entity_mgr_;
  PendingRpcRegistry rpc_registry_;
  BaseAppNativeProvider* native_provider_{nullptr};  // owned by ScriptApp
  Channel* dbapp_channel_{nullptr};
  Channel* dbappmgr_channel_{nullptr};
  std::unordered_map<Address, Channel*> dbapp_channels_;
  std::vector<dbappmgr::ShardEntry> dbapp_shard_table_;
  uint32_t dbapp_shard_table_version_{0};
  uint32_t next_dbapp_shard_table_request_id_{1};
  Channel* baseappmgr_channel_{nullptr};
  // Per-entity routing (which CellApp owns its Real) lives on
  // BaseEntity.cell_addr_; registry handles Birth/Death + self-filter.
  CellAppPeerRegistry cellapp_peers_;

  Channel* cellappmgr_channel_{nullptr};
  uint32_t next_space_request_id_{1};
  struct PendingSpaceCreate {
    SpaceID space_id;
    SpaceCreatedCallback callback;
    IntrusivePtr<Channel> channel;
    TimePoint started_at;
  };
  std::unordered_map<uint32_t, PendingSpaceCreate> pending_space_creates_;
  void FailPendingSpaceCreates();
  // space_id to space master type name. Lookup populates CreateSpaceRequest.
  std::unordered_map<SpaceID, std::string> space_master_types_;

  // Cache of {space_id to primary cell host} from SpaceCreatedResult; lets
  // CreateBaseEntityFromScript skip re-asking the mgr on subsequent entities.
  std::unordered_map<SpaceID, Address> known_space_hosts_;
  // Latest flattened BSP per space; replayed to a freshly-attached client.
  std::unordered_map<SpaceID, baseapp::SpaceBspGeometry> latest_space_bsp_;
  std::unordered_map<SpaceID, std::vector<cellapp::CreateCellEntity>>
      pending_cell_entity_creates_;
  std::unordered_map<SpaceID, std::vector<cellapp::SpawnLocalEntity>>
      pending_spawn_local_entities_;
  std::unordered_set<SpaceID> in_flight_space_requests_;

  uint32_t app_id_{0};

  struct PendingLogin {
    std::array<uint8_t, 32> entity_def_digest{};
    uint32_t login_request_id{0};
    Address loginapp_addr;
    uint16_t type_id{0};
    DatabaseID dbid{kInvalidDBID};
    std::optional<CheckoutInfo> checkout_owner{};
    std::array<std::byte, 16> checkout_operation_id{};
    uint32_t checkout_request_id{0};
    uint32_t checkout_app_id{0};
    SessionKey session_key;
    TimePoint created_at{};
    TimePoint force_logoff_sent_at{};
    TimePoint next_force_logoff_retry_at{};
    Address force_logoff_holder_addr;
    uint8_t force_logoff_retry_count{0};
    bool waiting_for_remote_force_logoff_ack{false};
    bool reply_sent{false};
    bool blob_prefetched{false};
    Address checkout_target_addr;
    uint8_t checkout_retry_count{0};
    bool checkout_awaiting_ack{false};
    bool checkout_retry_pending{false};
    bool checkout_outcome_unknown{false};
    TimePoint next_checkout_retry_at{};
    std::vector<std::byte> entity_blob;
    // Plumbed through so account_entity_index_ can be populated when the
    // entity finally binds to a client in OnClientAuthenticate.
    std::string username;
  };
  struct LoginSessionCheck {
    Address loginapp_addr;
    uint32_t login_request_id{0};
    DatabaseID dbid{kInvalidDBID};
    CancellationSource cancel;
  };
  std::unordered_map<uint64_t, LoginSessionCheck> login_session_checks_;
  std::unordered_map<uint32_t, PendingLogin> pending_logins_;
  uint32_t next_prepare_request_id_{1};

  std::unordered_map<uint32_t, PendingLogin> pending_force_logoffs_;
  struct PendingWriteToDb {
    EntityID entity_id{kInvalidEntityID};
    DatabaseID dbid{kInvalidDBID};
    uint64_t reply_request_id{0};
    dbapp::WriteEntity message;
    Address target_addr;
    TimePoint created_at{};
    TimePoint next_retry_at{};
    uint8_t retry_count{0};
    bool awaiting_ack{false};
    bool outcome_unknown{false};
    bool resolution_stalled{false};
    bool logoff_after_write{false};
  };
  struct PendingLogoffWrite {
    uint32_t continuation_request_id{0};
    EntityID entity_id{kInvalidEntityID};
    DatabaseID dbid{kInvalidDBID};
    uint16_t type_id{0};
    dbapp::WriteEntity message;
    Address target_addr;
    TimePoint created_at{};
    TimePoint next_retry_at{};
    uint8_t retry_count{0};
    bool awaiting_ack{false};
    bool retry_pending{false};
    bool outcome_unknown{false};
    bool resolution_stalled{false};
  };
  struct PendingRemoteForceLogoffAck {
    Address reply_addr;
    uint32_t request_id{0};
  };
  struct DeferredLoginCheckout {
    PendingLogin pending;
    DatabaseID dbid{kInvalidDBID};
    uint16_t type_id{0};
    std::vector<std::byte> blob;
  };
  struct DetachedProxyState {
    TimePoint detached_at{};
    TimePoint detached_until{};
  };
  struct PreparedLoginEntity {
    EntityID entity_id{kInvalidEntityID};
    DatabaseID dbid{kInvalidDBID};
    uint16_t type_id{0};
    TimePoint prepared_at{};
    // Bound into account_entity_index_ when OnClientAuthenticate succeeds.
    std::string username;
    std::optional<CheckoutInfo> checkout_owner{};
  };
  struct PendingDbCheckout {
    dbapp::CheckoutEntity request;
    DatabaseID resolved_dbid{kInvalidDBID};
    Address target_addr;
    TimePoint created_at{};
    TimePoint next_retry_at{};
    uint8_t retry_count{0};
    bool awaiting_ack{false};
    bool outcome_unknown{false};
    bool stalled{false};
  };
  struct LoadSnapshot {
    uint32_t entity_count{0};
    uint32_t proxy_count{0};
    uint32_t pending_prepare_count{0};
    uint32_t pending_force_logoff_count{0};
    uint32_t detached_proxy_count{0};
    uint32_t logoff_in_flight_count{0};
    uint32_t deferred_login_count{0};
  };
  struct LoadTracker {
    void MarkTickStarted();
    void ObserveTickComplete(int update_hertz, const LoadSnapshot& snapshot);
    [[nodiscard]] auto BuildReport(uint32_t app_id, const LoadSnapshot& snapshot) const
        -> baseappmgr::InformLoad;
    [[nodiscard]] auto CurrentLoad() const -> float { return load_; }

   private:
    float load_{0.0f};
    TimePoint tick_started_{};
  };
  std::unordered_map<uint32_t, PendingLogoffWrite> pending_logoff_writes_;
  std::unordered_map<uint32_t, PendingWriteToDb> pending_write_to_db_;
  std::unordered_map<EntityID, std::vector<PendingRemoteForceLogoffAck>>
      pending_remote_force_logoff_acks_;
  std::unordered_map<EntityID, std::vector<DeferredLoginCheckout>> deferred_login_checkouts_;
  std::unordered_map<EntityID, DetachedProxyState> detached_proxies_;
  std::unordered_map<uint32_t, PreparedLoginEntity> prepared_login_entities_;
  std::unordered_map<EntityID, uint32_t> prepared_login_requests_by_entity_;
  std::unordered_map<uint32_t, PendingDbCheckout> canceled_checkouts_;
  std::unordered_map<uint32_t, PendingDbCheckout> pending_entity_checkouts_;
  std::unordered_map<EntityID, std::vector<uint32_t>> pending_local_force_logoff_waiters_;
  std::unordered_set<EntityID> logoff_entities_in_flight_;
  std::unordered_set<DatabaseID> active_login_dbids_;
  std::unordered_map<DatabaseID, PendingLogin> queued_logins_;
  std::unordered_map<EntityID, Address> entity_client_index_;
  std::unordered_map<Address, EntityID> client_entity_index_;
  // Single-session-per-account: identity -> live owner entity, kept so a
  // same-name relogin can serialize against the prior session's teardown.
  std::unordered_map<std::string, EntityID> account_entity_index_;
  std::unordered_map<EntityID, std::string> account_index_reverse_;
  // Pending DestroyCellEntityAck waiters keyed by the request_id stamped
  // into the outgoing DestroyCellEntity; callback receives success.
  std::unordered_map<uint32_t, std::function<void(bool)>> destroy_ack_waiters_;
  uint32_t next_destroy_request_id_{1};
  struct DestroyInFlight {
    EntityID entity_id{kInvalidEntityID};
    TimePoint started_at{};
    std::vector<PendingLogin> queued;
    // 0 when no ack waiter was allocated (no cell or no channel).
    uint32_t ack_request_id{0};
  };
  // Same-name logins park here while the prior session's cellapp Avatar
  // is being torn down; drained on DestroyCellEntityAck or timeout.
  std::unordered_map<std::string, DestroyInFlight> destroys_in_flight_;
  static constexpr std::chrono::milliseconds kDestroyAckTimeout{1000};
  static constexpr std::size_t kMaxQueuedPerUsername = 4;
  std::unordered_map<Address, DeltaForwarder> client_delta_forwarders_;
  // SetAoIRadius issued before the cell ack lands is replayed in OnCellEntityCreated.
  std::unordered_map<EntityID, std::pair<float, float>> pending_aoi_radius_;

  // Client ClientCellRpc that arrived before CurrentCell; replayed there.
  // Capped per entity so a partitioned cellapp can't grow this unboundedly.
  struct PendingClientRpc {
    EntityID source_entity_id;
    uint32_t rpc_id;
    std::vector<std::byte> payload;
    uint64_t trace_id;
  };
  std::unordered_map<EntityID, std::vector<PendingClientRpc>> pending_client_rpcs_;
  static constexpr std::size_t kMaxPendingClientRpcsPerEntity = 64;

  struct RpcRateBucket {
    double tokens{0.0};
    std::chrono::steady_clock::time_point last_refill{};
    uint32_t consecutive_violations{0};
  };
  struct MovementInputSeqState {
    uint32_t newest_seq{0};
    bool initialized{false};
  };
  struct MovementAckRelayState {
    uint32_t acked_input_seq{0};
    uint32_t server_tick{0};
    uint32_t cell_epoch{0};
    uint32_t large_correction_streak{0};
    uint32_t reported_input_seq{0};
    uint32_t reported_server_tick{0};
    bool initialized{false};
    bool report_initialized{false};
  };
  enum class MovementInputSeqResult {
    kAccepted,
    kStale,
    kGap,
  };
  std::unordered_map<Address, RpcRateBucket> rpc_rate_buckets_;
  std::unordered_map<Address, RpcRateBucket> movement_rate_buckets_;
  std::unordered_map<Address, MovementInputSeqState> movement_input_seq_;
  std::unordered_map<EntityID, MovementAckRelayState> movement_ack_relay_state_;
  uint64_t delta_bytes_sent_total_{0};
  uint64_t delta_bytes_deferred_total_{0};
  uint64_t reliable_delta_bytes_sent_total_{0};
  uint64_t reliable_delta_messages_sent_total_{0};
  uint64_t baseline_messages_sent_total_{0};
  uint64_t baseline_bytes_sent_total_{0};
  uint64_t baseline_tick_counter_{0};
  static constexpr uint32_t kDeltaBudgetPerTick = 16 * 1024;  // 16 KB per client per tick
  // Reliable baseline cadence (~4s @ 30Hz / 12s @ 10Hz); unreliable 0xF001
  // path's stale frame is superseded by next frame so cadence is loose.
  static constexpr uint64_t kBaselineInterval = 120;
  uint64_t auth_success_total_{0};
  uint64_t auth_fail_total_{0};
  uint64_t force_logoff_total_{0};
  uint64_t fast_relogin_total_{0};
  uint64_t detached_relogin_total_{0};
  uint64_t canceled_checkout_total_{0};
  uint64_t prepared_login_timeout_total_{0};
  // Accumulated client-reported reliable-delta gap count.
  uint64_t client_event_seq_gaps_total_{0};
  uint64_t client_base_rpc_received_total_{0};
  uint64_t client_cell_rpc_received_total_{0};
  uint64_t cell_rpc_forward_received_total_{0};
  uint64_t broadcast_rpc_received_total_{0};
  uint64_t client_rpc_dropped_oversize_total_{0};
  uint64_t client_rpc_dropped_unknown_total_{0};
  uint64_t client_rpc_dropped_unauthorized_total_{0};
  uint64_t client_rpc_dropped_rate_total_{0};
  uint64_t movement_input_packets_total_{0};
  uint64_t movement_input_forwarded_total_{0};
  uint64_t movement_input_dropped_total_{0};
  uint64_t movement_input_rate_limited_total_{0};
  uint64_t movement_input_invalid_dropped_total_{0};
  uint64_t movement_input_stale_dropped_total_{0};
  uint64_t movement_input_seq_gap_dropped_total_{0};
  uint64_t movement_ack_sent_total_{0};
  uint64_t movement_ack_stale_dropped_total_{0};
  uint64_t movement_correction_tier1_total_{0};
  uint64_t movement_correction_tier2_total_{0};
  uint64_t movement_correction_snap_total_{0};
  uint64_t movement_correction_suspicious_total_{0};
  uint64_t movement_correction_report_total_{0};
  uint64_t movement_correction_report_dropped_total_{0};
  uint64_t cellapp_death_notifications_total_{0};
  uint64_t cellapp_death_restore_scheduled_total_{0};
  uint64_t cellapp_death_restore_payload_scheduled_total_{0};
  uint64_t cellapp_death_restore_ghost_backup_scheduled_total_{0};
  uint64_t cellapp_death_restored_total_{0};
  uint64_t cellapp_death_lost_total_{0};
  uint64_t cellapp_death_restore_timeouts_total_{0};
  uint64_t cellapp_death_restore_last_elapsed_ms_{0};
  uint64_t cellapp_death_restore_max_elapsed_ms_{0};
  struct PendingCellAppDeathRestore {
    TimePoint started_at{};
    Address target_addr;
  };
  std::unordered_map<EntityID, PendingCellAppDeathRestore> pending_cellapp_death_restores_;

  LatencyHistogram prepare_login_latency_;
  LatencyHistogram authenticate_latency_;
  LatencyHistogram force_logoff_latency_;
  LoadTracker load_tracker_{};
  static constexpr Duration kForceLogoffRetryBaseDelay = std::chrono::milliseconds(250);
  static constexpr Duration kForceLogoffRetryMaxDelay = std::chrono::seconds(2);
  static constexpr Duration kPendingTimeout = std::chrono::seconds(8);
  static constexpr Duration kPreparedLoginTimeout = std::chrono::seconds(10);
  static constexpr Duration kCellAppDeathRestoreTimeout = std::chrono::seconds(5);
  // One shortline reconnect window - longer adds stale-proxy pressure with
  // no fast-path benefit.
  static constexpr Duration kDetachedProxyGrace = std::chrono::milliseconds(1500);
  static constexpr float kLoadSmoothingBias = 0.25f;

  void CleanupExpiredPendingRequests();
  void FailAllDbappPendingRequests(std::string_view reason, bool fail_write_to_db = true,
                                   bool preserve_disconnected_logins = false,
                                   bool preserve_disconnected_logoffs = false);
  void CompletePendingLogoffWrite(uint32_t request_id, PendingLogoffWrite pending, bool success,
                                  std::string_view error);
  void FailPendingPrepareLogin(PendingLogin& pending, std::string_view reason);
  void FailPendingPrepareLogin(uint32_t request_id, std::string_view reason);
  void FailPendingForceLogoff(PendingLogin& pending, std::string_view reason);
  void FailPendingForceLogoff(uint32_t request_id, std::string_view reason);
  void ScheduleForceLogoffRetry(PendingLogin& pending, TimePoint now);
  void RetryStalledForceLogoff(uint32_t request_id);
  void ReleaseCheckout(DatabaseID dbid, uint16_t type_id,
                       std::optional<CheckoutInfo> owner = std::nullopt);
  void RetryCheckins();
  void SendCheckin(uint32_t request_id);
  void OnCheckinEntityAck(const Address& src, const dbapp::CheckinEntityAck& msg);
  struct PendingCheckin {
    dbapp::CheckinEntity request;
    Address target;
    TimePoint next_retry{};
    uint8_t attempts{0};
    bool stalled{false};
  };
  std::unordered_map<uint32_t, PendingCheckin> pending_checkins_;
  void CancelInflightCheckout(uint32_t request_id, const PendingLogin& pending);
  void ReleaseLoginCheckout(DatabaseID dbid, uint16_t type_id, const PendingLogin& pending);
  auto SendAbortCheckout(PendingDbCheckout& pending) -> bool;
  auto SendEntityCheckout(PendingDbCheckout& pending) -> bool;
  auto AllocatePrepareRequestId() -> uint32_t;
  void RetryEntityCheckouts();
  void RetryCanceledCheckouts();
  void OnCheckoutEntityAck(const Address& src, const dbapp::CheckoutEntityAck& msg);
  void OnAbortCheckoutAck(const Address& src, const dbapp::AbortCheckoutAck& msg);
  void CancelPrepareLogin(uint32_t login_request_id, DatabaseID dbid);
  [[nodiscard]] auto RollbackPreparedLoginEntity(uint32_t login_request_id) -> bool;
  void ClearPreparedLoginEntity(EntityID entity_id);
  [[nodiscard]] auto RetryLoginAfterCheckoutConflict(PendingLogin pending, DatabaseID dbid,
                                                     const Address& holder_addr) -> bool;
  [[nodiscard]] auto RestoreManagedEntity(EntityID entity_id, uint16_t type_id, DatabaseID dbid,
                                          std::span<const std::byte> blob) -> bool;
  [[nodiscard]] auto NotifyManagedEntityDestroyed(EntityID entity_id, std::string_view context)
      -> bool;
  auto CaptureEntitySnapshot(EntityID entity_id, std::vector<std::byte>& out) -> bool;
  [[nodiscard]] auto RotateProxySession(EntityID entity_id, const SessionKey& session_key) -> bool;
  [[nodiscard]] auto TryCompleteLocalRelogin(PendingLogin pending) -> bool;
  void EnterDetachedGrace(EntityID entity_id);
  void ClearDetachedGrace(EntityID entity_id);
  [[nodiscard]] auto DeferredLoginCheckoutCount() const -> std::size_t;
  void CompletePrepareLoginFromCheckout(PendingLogin pending, DatabaseID dbid, uint16_t type_id,
                                        std::span<const std::byte> blob);
  void DeferPrepareLoginFromCheckout(EntityID blocking_entity_id, PendingLogin pending,
                                     DatabaseID dbid, uint16_t type_id,
                                     std::span<const std::byte> blob);
  void FailDeferredPrepareLogins(EntityID blocking_entity_id, std::string_view reason,
                                 std::vector<DatabaseID>* finished_dbids = nullptr);
  [[nodiscard]] auto ResumeDeferredPrepareLogins(EntityID blocking_entity_id) -> bool;
  void SubmitPrepareLogin(PendingLogin pending);
  void DispatchPrepareLogin(PendingLogin pending);
  void FinishLoginFlow(DatabaseID dbid);
  void StartDisconnectLogoff(EntityID entity_id);
  void FlushRemoteForceLogoffAcks(EntityID entity_id, bool success);
  void FlushAllRemoteForceLogoffAcks(bool success);
  void BeginLogoffPersist(EntityID entity_id, DatabaseID dbid, uint16_t type_id,
                          uint32_t continuation_request_id);
  void BeginForceLogoffPersist(uint32_t force_request_id, EntityID entity_id);
  void ContinueLoginAfterForceLogoff(uint32_t force_request_id);
  [[nodiscard]] auto FinalizeForceLogoff(EntityID entity_id,
                                          uint32_t cell_ack_request_id = 0) -> bool;
  void ProcessForceLogoffRequest(const baseapp::ForceLogoff& msg);
  auto ResolveInternalChannel(const Address& addr) -> Channel*;
  auto ResolveClientChannel(EntityID entity_id) -> Channel*;
  auto BindClient(EntityID entity_id, const Address& client_addr) -> bool;
  void UnbindClient(EntityID entity_id);
  void OnExternalClientDisconnect(Channel& ch);
  // Single-session-per-account index helpers.
  void RegisterAccountOwner(const std::string& username, EntityID entity_id);
  void TransferAccountOwner(EntityID old_entity_id, EntityID new_entity_id);
  void UnregisterAccountOwner(EntityID entity_id);
  // Pairs a callback with a fresh request_id for DestroyCellEntity; returns
  // 0 when no callback is supplied so the message can fire and forget.
  auto AllocateDestroyAckId(std::function<void(bool)> on_ack) -> uint32_t;
  void OnDestroyCellEntityAck(const cellapp::DestroyCellEntityAck& msg);
  void ForceLogoffUsernameOwner(const std::string& username);
  void QueuePendingLoginAwaitingDestroy(const std::string& username, PendingLogin pending);
  void OnUsernameDestroyComplete(const std::string& username);
  void SweepStaleDestroysInFlight();
  void SweepCellAppDeathRestoreTimeouts();
  [[nodiscard]] auto BuildCellAppDeathRestoreStatus() const -> std::string;
  [[nodiscard]] auto BuildCellAppRouteSummary() const -> std::string;
  auto RecordCellAppDeathRestoreElapsed(const PendingCellAppDeathRestore& pending,
                                        TimePoint now) -> uint64_t;
  // Returns true when one token is consumed; false when the bucket is empty
  // and the call should be dropped at the message boundary.
  bool ConsumeRpcRateToken(const Address& client_addr);
  bool ConsumeMovementRateToken(const Address& client_addr);
  auto TrackMovementInputSequence(const Address& client_addr,
                                  const baseapp::ClientMovementInput& msg)
      -> MovementInputSeqResult;
  void SeedMovementInputSequenceFromAck(const Address& client_addr, uint32_t acked_input_seq);
  void RecordMovementCorrectionFlags(MovementAckRelayState& ack_state, uint16_t flags);
  void SendPrepareLoginResult(const Address& reply_addr, const login::PrepareLoginResult& msg);
};

}  // namespace atlas

#endif  // ATLAS_SERVER_BASEAPP_BASEAPP_H_
