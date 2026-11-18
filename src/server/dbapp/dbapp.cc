#include "dbapp.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <utility>

#include "db/entity_blob.h"
#include "db/write_operation.h"
#include "foundation/clock.h"
#include "foundation/log.h"
#include "loginapp/login_messages.h"
#include "network/channel.h"
#include "network/machined_types.h"
#include "network/reliable_udp.h"
#include "platform/password_hash.h"
#include "server/db_shard_routing.h"
#include "server/watcher.h"

namespace atlas {

namespace {

constexpr Duration kDbAppMgrLoadReportInterval = std::chrono::seconds(1);
constexpr int kCreateDbidDuplicateRetryLimit = 16;

auto SameCheckoutRequest(const dbapp::CheckoutEntity& a, const dbapp::CheckoutEntity& b) -> bool {
  return a.operation_id == b.operation_id && a.mode == b.mode && a.type_id == b.type_id &&
         a.dbid == b.dbid && a.identifier == b.identifier && a.entity_id == b.entity_id &&
         a.owner_app_id == b.owner_app_id && a.owner_process_instance == b.owner_process_instance;
}

}  // namespace

DBApp::~DBApp() {
  StopAuthentication();
  authentication_tasks_.reset();
}

auto DBApp::Run(int argc, char* argv[]) -> int {
  EventDispatcher dispatcher;
  NetworkInterface network(dispatcher);
  DBApp app(dispatcher, network);
  return app.RunApp(argc, argv);
}

auto DBApp::Init(int argc, char* argv[]) -> bool {
  if (!ManagerApp::Init(argc, argv)) return false;

  const auto& cfg = Config();

  if (cfg.entitydef_bin_path.empty()) {
    ATLAS_LOG_ERROR(
        "DBApp requires --entitydef-bin-path; export the matching entity assembly "
        "with Atlas.Tools.DefDump before starting the database process");
    return false;
  }
  entity_defs_.emplace();
  auto result = entity_defs_->RegisterFromBinaryFile(cfg.entitydef_bin_path);
  if (!result) {
    ATLAS_LOG_ERROR("DBApp: failed to load entity_defs from '{}': {}",
                    cfg.entitydef_bin_path.string(), result.Error().Message());
    return false;
  }
  if (entity_defs_->TypeCount() == 0 ||
      std::ranges::all_of(entity_defs_->Digest(), [](uint8_t byte) { return byte == 0; })) {
    ATLAS_LOG_ERROR(
        "DBApp: entity definitions require entity types and a nonzero definition "
        "digest; rebuild and export the matching game entity assembly");
    return false;
  }
  ATLAS_LOG_INFO("DBApp: loaded entity_defs from '{}' (structs={}, components={}, types={})",
                 cfg.entitydef_bin_path.string(), result->structs, result->components,
                 result->types);

  auto db_cfg = BuildDbConfig();
  database_ = CreateDatabase(db_cfg);
  if (!database_) {
    ATLAS_LOG_ERROR("DBApp: unknown database backend '{}'", db_cfg.type);
    return false;
  }

  auto startup_result = database_->Startup(db_cfg, *entity_defs_);
  if (!startup_result) {
    ATLAS_LOG_ERROR("DBApp: database startup failed: {}", startup_result.Error().Message());
    return false;
  }
  database_->SetDeferredMode(true);

  auto& table = Network().InterfaceTable();

  (void)table.RegisterTypedHandler<dbapp::WriteEntity>(
      [this](const Address& src, Channel* ch, const dbapp::WriteEntity& msg) {
        OnWriteEntity(src, ch, msg);
      });

  (void)table.RegisterTypedHandler<dbapp::AccountSessionRequest>(
      [this](const Address& src, Channel* ch, const dbapp::AccountSessionRequest& msg) {
        OnAccountSession(src, ch, msg);
      });

  (void)table.RegisterTypedHandler<dbapp::CheckoutEntity>(
      [this](const Address& src, Channel* ch, const dbapp::CheckoutEntity& msg) {
        OnCheckoutEntity(src, ch, msg);
      });

  (void)table.RegisterTypedHandler<dbapp::CheckinEntity>(
      [this](const Address& src, Channel* ch, const dbapp::CheckinEntity& msg) {
        OnCheckinEntity(src, ch, msg);
      });

  (void)table.RegisterTypedHandler<dbapp::DeleteEntity>(
      [this](const Address& src, Channel* ch, const dbapp::DeleteEntity& msg) {
        OnDeleteEntity(src, ch, msg);
      });

  (void)table.RegisterTypedHandler<dbapp::LookupEntity>(
      [this](const Address& src, Channel* ch, const dbapp::LookupEntity& msg) {
        OnLookupEntity(src, ch, msg);
      });

  (void)table.RegisterTypedHandler<dbapp::AbortCheckout>(
      [this](const Address& src, Channel* ch, const dbapp::AbortCheckout& msg) {
        OnAbortCheckout(src, ch, msg);
      });

  (void)table.RegisterTypedHandler<login::AuthLogin>(
      [this](const Address& src, Channel* ch, const login::AuthLogin& msg) {
        OnAuthLogin(src, ch, msg);
      });

  (void)table.RegisterTypedHandler<dbapp::GetEntityIds>(
      [this](const Address& src, Channel* ch, const dbapp::GetEntityIds& msg) {
        OnGetEntityIds(src, ch, msg);
      });

  (void)table.RegisterTypedHandler<dbapp::PutEntityIds>(
      [this](const Address& src, Channel* ch, const dbapp::PutEntityIds& msg) {
        OnPutEntityIds(src, ch, msg);
      });

  (void)table.RegisterTypedHandler<dbappmgr::RegisterDbAppAck>(
      [this](const Address& src, Channel* ch, const dbappmgr::RegisterDbAppAck& msg) {
        OnRegisterDbAppAck(src, ch, msg);
      });

  (void)table.RegisterTypedHandler<dbappmgr::ShardTableUpdate>(
      [this](const Address& src, Channel* ch, const dbappmgr::ShardTableUpdate& msg) {
        OnShardTableUpdate(src, ch, msg);
      });

  GetMachinedClient().Subscribe(
      machined::ListenerType::kBoth, ProcessType::kBaseApp,
      [this](const machined::BirthNotification& notif) {
        if (notif.process_instance != 0)
          baseapp_instances_[notif.internal_addr] = notif.process_instance;
      },
      [this](const machined::DeathNotification& notif) {
        auto found = baseapp_instances_.find(notif.internal_addr);
        if (found != baseapp_instances_.end() && found->second == notif.process_instance)
          baseapp_instances_.erase(found);
        OnBaseappDeath(notif.internal_addr, notif.process_instance);
      });
  GetMachinedClient().Subscribe(
      machined::ListenerType::kBoth, ProcessType::kDbAppMgr,
      [this](const machined::BirthNotification& notif) { OnDbAppMgrBirth(notif); },
      [this](const machined::DeathNotification& notif) { OnDbAppMgrDeath(notif); });

  auto_create_accounts_ = cfg.auto_create_accounts;
  account_type_id_ = cfg.account_type_id;

  id_allocator_ = std::make_unique<EntityIdAllocator>(*database_);

  ATLAS_LOG_INFO("DBApp: initialised (backend={}, auto_create={}, account_type={})", db_cfg.type,
                 auto_create_accounts_, account_type_id_);
  return true;
}

void DBApp::BeginShutdown() {
  ManagerApp::BeginShutdown();
  StopAuthentication();
  if (database_) database_->BeginDrain();
}

void DBApp::OnShutdownTick() {
  if (database_) database_->ProcessResults();
}

auto DBApp::PendingShutdownOperations() const -> std::size_t {
  return ManagerApp::PendingShutdownOperations() +
         (database_ ? database_->PendingOperations() : 0) + pending_write_requests_.size() +
         pending_checkout_requests_.size() + account_session_requests_ +
         process_retirements_.size() + active_authentications_ + pending_authentications_.size();
}

void DBApp::LogShutdownPending() const {
  ManagerApp::LogShutdownPending();
  ATLAS_LOG_ERROR(
      "DBApp: shutdown database_operations={} retirements={} auth_active={} auth_queued={}",
      database_ ? database_->PendingOperations() : 0, process_retirements_.size(),
      active_authentications_, pending_authentications_.size());
}

void DBApp::Fini() {
  StopAuthentication();
  authentication_tasks_.reset();
  if (database_) {
    database_->ProcessResults();
    database_->Shutdown();
    database_.reset();
    id_allocator_.reset();
  }
  ManagerApp::Fini();
}

void DBApp::OnTickComplete() {
  ManagerApp::OnTickComplete();
  if (database_) {
    database_->ProcessResults();
  }
  std::vector<std::pair<Address, uint64_t>> ready;
  for (const auto& process : process_retirements_) {
    if (!process.in_flight && Clock::now() >= process.next_attempt)
      ready.emplace_back(process.addr, process.instance);
  }
  for (const auto& [addr, instance] : ready) RetireBaseappProcess(addr, instance);
  ReportLoadToDbAppMgr();
}

auto DBApp::AdmissionBlockReason() const -> std::string_view {
  if (!database_ || !entity_defs_) return "database_uninitialized";
  if (!HasAuthoritativeShardTable()) return "shards_unregistered";
  if (!create_dbid_recovery_pending_.empty()) return "allocator_recovering";
  if (database_->PendingOperations() >= IDatabase::kMaxPendingOperations)
    return "database_queue_full";
  return {};
}

void DBApp::RegisterWatchers() {
  ManagerApp::RegisterWatchers();
  auto& reg = GetWatcherRegistry();

  reg.Add<std::size_t>("dbapp/pending_operations",
                       [this] { return database_ ? database_->PendingOperations() : 0; });
  reg.Add<std::size_t>("dbapp/operation_limit", [] { return IDatabase::kMaxPendingOperations; });
  reg.Add<std::size_t>("dbapp/pending_write_requests",
                       [this] { return pending_write_requests_.size(); });
  reg.Add<std::size_t>("dbapp/account_session_requests",
                       [this] { return account_session_requests_; });
  reg.Add<std::size_t>("dbapp/process_retirements", [this] { return process_retirements_.size(); });
  reg.Add<std::size_t>("dbapp/allocator_recoveries",
                       [this] { return create_dbid_recovery_pending_.size(); });
  reg.Add<std::size_t>("dbapp/auth_active", [this] { return active_authentications_; });
  reg.Add<std::size_t>("dbapp/auth_queued", [this] { return pending_authentications_.size(); });

  reg.Add<std::string>("dbapp/entity_defs_loaded", std::function<std::string()>{[this]() {
                         return (entity_defs_.has_value() && entity_defs_->TypeCount() > 0) ? "yes"
                                                                                            : "no";
                       }});

  reg.Add<std::string>(
      "dbapp/entity_type_count", std::function<std::string()>{[this]() {
        return std::to_string(entity_defs_.has_value() ? entity_defs_->TypeCount() : 0u);
      }});

  reg.Add<std::string>("dbapp/checkouts", std::function<std::string()>{[this]() {
                         return std::to_string(checkout_mgr_.size());
                       }});
  reg.Add<std::size_t>("dbapp/pending_checkout_request_count", std::function<std::size_t()>([this] {
                         return pending_checkout_requests_.size();
                       }));
  reg.Add<uint64_t>("dbapp/abort_checkout_total",
                    std::function<uint64_t()>([this] { return abort_checkout_total_; }));
  reg.Add<uint64_t>("dbapp/abort_checkout_pending_hit_total", std::function<uint64_t()>([this] {
                      return abort_checkout_pending_hit_total_;
                    }));
  reg.Add<uint64_t>("dbapp/abort_checkout_late_hit_total",
                    std::function<uint64_t()>([this] { return abort_checkout_late_hit_total_; }));

  reg.Add<uint32_t>("dbapp/dbapp_id", std::function<uint32_t()>([this] { return dbapp_id_; }));
  reg.Add<uint32_t>("dbapp/shard_table_version",
                    std::function<uint32_t()>([this] { return shard_table_version_; }));
  reg.Add<std::size_t>("dbapp/shard_count",
                       std::function<std::size_t()>([this] { return shard_table_.size(); }));
  RegisterLatencyWatchers(reg, "dbapp/checkout_reply_latency", checkout_reply_latency_);
  RegisterLatencyWatchers(reg, "dbapp/write_reply_latency", write_reply_latency_);
}

auto DBApp::BuildDbConfig() const -> DatabaseConfig {
  const auto& cfg = Config();
  DatabaseConfig db_cfg;
  db_cfg.type = cfg.db_type;
  db_cfg.mysql_host = cfg.db_mysql_host;
  db_cfg.mysql_port = cfg.db_mysql_port;
  db_cfg.mysql_user = cfg.db_mysql_user;
  db_cfg.mysql_password = cfg.db_mysql_password;
  db_cfg.mysql_database = cfg.db_mysql_database;
  db_cfg.mysql_database_id = cfg.db_mysql_database_id;
  db_cfg.mysql_ssl_ca = cfg.db_mysql_ssl_ca;
  db_cfg.mysql_initialize = cfg.db_mysql_initialize;
  db_cfg.mysql_pool_size = cfg.db_mysql_pool_size;
  return db_cfg;
}

auto DBApp::ResolveReplyChannel(const Address& addr) -> Channel* {
  if (auto* existing = Network().FindChannel(addr)) {
    return existing;
  }

  auto result = Network().ConnectRudpNocwnd(addr);
  if (!result) {
    ATLAS_LOG_WARNING("DBApp: failed to resolve reply channel {}:{}", addr.Ip(), addr.Port());
    return nullptr;
  }

  return static_cast<Channel*>(*result);
}

auto DBApp::RequestCacheKeyHash::operator()(const RequestCacheKey& key) const noexcept
    -> std::size_t {
  auto h = std::hash<Address>{}(key.reply_addr);
  h ^= static_cast<std::size_t>(key.request_id) + std::size_t{0x9E3779B97F4A7C15ULL} + (h << 6) +
       (h >> 2);
  for (auto byte : key.operation_id) h = (h ^ std::to_integer<uint8_t>(byte)) * 1099511628211ULL;
  return h;
}

auto DBApp::SendWriteAck(const RequestCacheKey& key, const dbapp::WriteEntityAck& ack,
                         Channel* fallback_ch) -> bool {
  auto* reply_ch = fallback_ch != nullptr ? fallback_ch : ResolveReplyChannel(key.reply_addr);
  if (reply_ch == nullptr) return false;
  if (auto r = reply_ch->SendMessage(ack); !r) {
    ATLAS_LOG_ERROR(
        "DBApp: WriteEntityAck dropped, dbid={} request_id={} success={} to {}: {} "
        "— durability boundary desync",
        ack.dbid, ack.request_id, ack.success, key.reply_addr.ToString(), r.Error().Message());
    return false;
  }
  return true;
}

auto DBApp::SendCheckoutAck(const RequestCacheKey& key, const dbapp::CheckoutEntityAck& ack,
                            Channel* fallback_ch) -> bool {
  auto* reply_ch = fallback_ch != nullptr ? fallback_ch : ResolveReplyChannel(key.reply_addr);
  if (reply_ch == nullptr) return false;
  if (auto r = reply_ch->SendMessage(ack); !r) {
    ATLAS_LOG_ERROR(
        "DBApp: CheckoutEntityAck dropped, dbid={} request_id={} status={} to {}: {} "
        "— DBID stays checked out, login wedge",
        ack.dbid, ack.request_id, static_cast<int>(ack.status), key.reply_addr.ToString(),
        r.Error().Message());
    return false;
  }
  return true;
}

void DBApp::RememberCheckoutAck(const RequestCacheKey& key, const dbapp::CheckoutEntity& request,
                                const dbapp::CheckoutEntityAck& ack) {
  auto [it, inserted] = checkout_ack_cache_.try_emplace(key, CachedCheckoutReply{request, ack});
  if (!inserted) {
    it->second = CachedCheckoutReply{request, ack};
    return;
  }
  checkout_ack_order_.push_back(key);
  while (checkout_ack_cache_.size() > kRequestAckCacheLimit && !checkout_ack_order_.empty()) {
    checkout_ack_cache_.erase(checkout_ack_order_.front());
    checkout_ack_order_.pop_front();
  }
}

void DBApp::OnDbAppMgrBirth(const machined::BirthNotification& msg) {
  if (dbappmgr_channel_ != nullptr && dbappmgr_channel_->RemoteAddress() == msg.internal_addr &&
      !dbappmgr_channel_->IsCondemned()) {
    RegisterWithDbAppMgr();
    return;
  }

  ATLAS_LOG_INFO("DBApp: DBAppMgr born at {}, registering", msg.internal_addr.ToString());
  auto ch = Network().ConnectRudpNocwnd(msg.internal_addr);
  if (!ch) {
    ATLAS_LOG_WARNING("DBApp: failed to connect to DBAppMgr at {}", msg.internal_addr.ToString());
    return;
  }
  dbappmgr_channel_ = static_cast<Channel*>(*ch);
  RegisterWithDbAppMgr();
}

void DBApp::OnDbAppMgrDeath(const machined::DeathNotification& msg) {
  if (dbappmgr_channel_ != nullptr && dbappmgr_channel_->RemoteAddress() != msg.internal_addr) {
    return;
  }
  if (msg.reason == 0) {
    ATLAS_LOG_INFO("DBApp: DBAppMgr deregistered");
  } else {
    ATLAS_LOG_WARNING("DBApp: DBAppMgr died");
  }
  dbappmgr_channel_ = nullptr;
}

void DBApp::OnRegisterDbAppAck(const Address& src, Channel* ch,
                               const dbappmgr::RegisterDbAppAck& msg) {
  if (dbappmgr_channel_ != nullptr && ch != nullptr && ch != dbappmgr_channel_) {
    ATLAS_LOG_WARNING("DBApp: ignoring RegisterDbAppAck from unexpected channel {}",
                      src.ToString());
    return;
  }
  if (!msg.success || msg.dbapp_id == 0) {
    ATLAS_LOG_ERROR("DBApp: DBAppMgr rejected registration from {}", src.ToString());
    return;
  }

  const uint32_t kPreviousDbAppId = dbapp_id_;
  const uint32_t kPreviousShardTableVersion = shard_table_version_;
  dbapp_id_ = msg.dbapp_id;
  const bool kDbAppIdChanged = kPreviousDbAppId != 0 && kPreviousDbAppId != msg.dbapp_id;
  const bool kApplyShardTable =
      kDbAppIdChanged || kPreviousShardTableVersion == 0 ||
      msg.shard_table_version > kPreviousShardTableVersion ||
      (msg.shard_table_version == kPreviousShardTableVersion && !msg.entries.empty());
  if (kApplyShardTable) {
    shard_table_version_ = msg.shard_table_version;
    shard_table_ = msg.entries;
    PruneCreateDbidAllocators();
    RecoverCreateDbidAllocators();
  }
  last_dbappmgr_load_report_at_ = {};
  ReportLoadToDbAppMgr();
  ATLAS_LOG_INFO("DBApp: registered with DBAppMgr as id={} shard_version={} shards={}", dbapp_id_,
                 shard_table_version_, shard_table_.size());
}

void DBApp::OnShardTableUpdate(const Address& src, Channel* ch,
                               const dbappmgr::ShardTableUpdate& msg) {
  if (dbappmgr_channel_ != nullptr && ch != nullptr && ch != dbappmgr_channel_) {
    ATLAS_LOG_WARNING("DBApp: ignoring ShardTableUpdate from unexpected channel {}",
                      src.ToString());
    return;
  }
  if (msg.version == 0 || msg.version <= shard_table_version_) return;

  shard_table_version_ = msg.version;
  shard_table_ = msg.entries;
  PruneCreateDbidAllocators();
  RecoverCreateDbidAllocators();
  ATLAS_LOG_INFO("DBApp: shard table updated version={} shards={}", shard_table_version_,
                 shard_table_.size());
}

void DBApp::RegisterWithDbAppMgr() {
  if (dbappmgr_channel_ == nullptr) return;
  if (Config().internal_port == 0) {
    ATLAS_LOG_WARNING("DBApp: cannot register with DBAppMgr without --internal-port");
    return;
  }

  dbappmgr::RegisterDbApp msg;
  msg.internal_addr = Address(0, Config().internal_port);
  msg.known_app_id = dbapp_id_;
  msg.known_shard_table_version = shard_table_version_;
  if (auto r = dbappmgr_channel_->SendMessage(msg); !r) {
    ATLAS_LOG_WARNING("DBApp: failed to send RegisterDbApp: {}", r.Error().Message());
    return;
  }
  ReportRecoverStateToDbAppMgr();
}

void DBApp::ReportRecoverStateToDbAppMgr() {
  if (dbappmgr_channel_ == nullptr || dbapp_id_ == 0 || shard_table_version_ == 0) return;

  dbappmgr::RecoverDBAppState msg;
  msg.dbapp_id = dbapp_id_;
  msg.shard_table_version = shard_table_version_;
  for (const auto& shard : shard_table_) {
    if (shard.dbapp_id != dbapp_id_) continue;
    auto entry = shard;
    entry.dbapp_addr = Address(0, Config().internal_port);
    msg.shards.push_back(entry);
  }
  if (msg.shards.empty()) return;

  if (auto r = dbappmgr_channel_->SendMessage(msg); !r) {
    ATLAS_LOG_WARNING("DBApp: failed to send RecoverDBAppState: {}", r.Error().Message());
  }
}

void DBApp::ReportLoadToDbAppMgr() {
  if (dbappmgr_channel_ == nullptr || dbapp_id_ == 0) return;
  const auto now = Clock::now();
  if (last_dbappmgr_load_report_at_ != TimePoint{} &&
      now - last_dbappmgr_load_report_at_ < kDbAppMgrLoadReportInterval) {
    return;
  }

  dbappmgr::InformLoad msg;
  msg.dbapp_id = dbapp_id_;
  msg.load = CurrentLoadFraction();
  msg.entity_count = static_cast<uint32_t>(checkout_mgr_.size());
  msg.pending_checkout_count = static_cast<uint32_t>(pending_checkout_requests_.size());
  msg.write_queue_depth = 0;
  if (auto r = dbappmgr_channel_->SendMessage(msg); !r) {
    ATLAS_LOG_WARNING("DBApp: failed to send InformLoad: {}", r.Error().Message());
    return;
  }
  last_dbappmgr_load_report_at_ = now;
}

auto DBApp::CurrentLoadFraction() const -> float {
  const auto expected = ExpectedTickPeriod();
  if (expected <= Duration{}) return 0.0f;
  const double load =
      static_cast<double>(LastTickWorkDuration().count()) / static_cast<double>(expected.count());
  return static_cast<float>(std::clamp(load, 0.0, 1.0));
}

auto DBApp::AcceptsShardTableVersion(uint32_t version) const -> bool {
  return version == 0 || shard_table_version_ == 0 || version == shard_table_version_;
}

auto DBApp::HasAuthoritativeShardTable() const -> bool {
  return dbapp_id_ != 0 && !shard_table_.empty();
}

auto DBApp::FindShard(DatabaseID dbid) const -> const dbappmgr::ShardEntry* {
  for (const auto& entry : shard_table_) {
    if (entry.low_dbid <= dbid && dbid < entry.high_dbid) return &entry;
  }
  return nullptr;
}

auto DBApp::AllocateCreateDbid(const dbappmgr::ShardEntry& shard) -> DatabaseID {
  if (shard.dbapp_id != dbapp_id_ || shard.is_retiring) return kInvalidDBID;
  if (create_dbid_recovery_pending_.contains(shard.low_dbid)) return kInvalidDBID;
  auto next_it = next_create_dbids_.find(shard.low_dbid);
  if (next_it == next_create_dbids_.end()) return kInvalidDBID;
  auto& next = next_it->second;
  if (next < shard.low_dbid) next = shard.low_dbid;
  if (next >= shard.high_dbid) return kInvalidDBID;
  return next++;
}

auto DBApp::AllocateCreateDbidForRoute(DatabaseID route_dbid) -> DatabaseID {
  const auto* shard = FindShard(route_dbid);
  if (shard == nullptr) return kInvalidDBID;
  return AllocateCreateDbid(*shard);
}

auto DBApp::AllocateCreateDbidFromOwnedShard() -> DatabaseID {
  for (const auto& entry : shard_table_) {
    if (entry.dbapp_id != dbapp_id_ || entry.is_retiring) continue;
    if (DatabaseID dbid = AllocateCreateDbid(entry); dbid != kInvalidDBID) return dbid;
  }
  return kInvalidDBID;
}

void DBApp::PruneCreateDbidAllocators() {
  for (auto it = next_create_dbids_.begin(); it != next_create_dbids_.end();) {
    const DatabaseID low = it->first;
    const auto shard_it =
        std::find_if(shard_table_.begin(), shard_table_.end(), [this, low](const auto& entry) {
          return entry.low_dbid == low && entry.dbapp_id == dbapp_id_ && !entry.is_retiring;
        });
    if (shard_it == shard_table_.end()) {
      it = next_create_dbids_.erase(it);
      continue;
    }
    if (it->second < shard_it->low_dbid) it->second = shard_it->low_dbid;
    ++it;
  }

  for (auto it = create_dbid_recovery_pending_.begin();
       it != create_dbid_recovery_pending_.end();) {
    const DatabaseID low = *it;
    const auto shard_it =
        std::find_if(shard_table_.begin(), shard_table_.end(), [this, low](const auto& entry) {
          return entry.low_dbid == low && entry.dbapp_id == dbapp_id_ && !entry.is_retiring;
        });
    if (shard_it == shard_table_.end()) {
      it = create_dbid_recovery_pending_.erase(it);
      continue;
    }
    ++it;
  }
}

void DBApp::RecoverCreateDbidAllocators() {
  if (!database_) return;
  for (const auto& entry : shard_table_) {
    if (entry.dbapp_id != dbapp_id_ || entry.is_retiring) continue;
    if (next_create_dbids_.contains(entry.low_dbid)) continue;
    if (create_dbid_recovery_pending_.contains(entry.low_dbid)) continue;
    RecoverCreateDbidAllocator(entry);
  }
}

void DBApp::RecoverCreateDbidAllocator(const dbappmgr::ShardEntry& shard) {
  if (!database_) return;
  create_dbid_recovery_pending_.insert(shard.low_dbid);
  database_->GetMaxDbidInRange(
      shard.low_dbid, shard.high_dbid,
      [this, low = shard.low_dbid, high = shard.high_dbid](DbidRangeResult result) {
        if (!result.success) {
          create_dbid_recovery_pending_.erase(low);
          ATLAS_LOG_ERROR("DBApp: failed to recover create DBID allocator for [{}, {}): {}", low,
                          high, result.error);
          return;
        }

        const auto shard_it = std::find_if(shard_table_.begin(), shard_table_.end(),
                                           [this, low, high](const auto& e) {
                                             return e.low_dbid == low && e.high_dbid == high &&
                                                    e.dbapp_id == dbapp_id_ && !e.is_retiring;
                                           });
        if (shard_it == shard_table_.end()) {
          create_dbid_recovery_pending_.erase(low);
          RecoverCreateDbidAllocators();
          return;
        }

        DatabaseID next = low;
        if (low <= result.max_dbid && result.max_dbid < high) {
          next = result.max_dbid + 1;
        }
        next_create_dbids_[low] = next;
        create_dbid_recovery_pending_.erase(low);
        ATLAS_LOG_INFO("DBApp: recovered create DBID allocator for [{}, {}) next={}", low, high,
                       next);
      });
}

void DBApp::PutCreateEntityWithRetry(const dbappmgr::ShardEntry& shard, DatabaseID dbid,
                                     uint16_t type_id, WriteFlags flags,
                                     std::vector<std::byte> blob, std::string identifier,
                                     std::optional<std::string> password_hash,
                                     std::function<void(PutResult)> callback,
                                     DatabaseWriteOperation operation) {
  auto final_callback = std::make_shared<std::function<void(PutResult)>>(std::move(callback));
  auto attempt = std::make_shared<std::function<void(DatabaseID, int)>>();
  std::weak_ptr<std::function<void(DatabaseID, int)>> weak_attempt = attempt;

  *attempt = [this, weak_attempt, shard, type_id, flags, blob = std::move(blob),
              identifier = std::move(identifier), password_hash = std::move(password_hash),
              final_callback, operation](DatabaseID attempt_dbid, int retries_left) {
    auto attempt_keepalive = weak_attempt.lock();
    auto on_put = [this, weak_attempt, attempt_keepalive, shard, retries_left,
                   final_callback](PutResult result) {
      if (!result.success && result.error_kind == DatabaseErrorKind::kDuplicateDbid &&
          retries_left > 0) {
        DatabaseID retry_dbid = AllocateCreateDbid(shard);
        if (retry_dbid != kInvalidDBID) {
          ATLAS_LOG_WARNING("DBApp: create DBID {} already exists, retrying with {}", result.dbid,
                            retry_dbid);
          if (auto attempt_fn = weak_attempt.lock()) {
            (*attempt_fn)(retry_dbid, retries_left - 1);
            return;
          }
        }
      }
      (*final_callback)(std::move(result));
    };

    if (password_hash.has_value()) {
      database_->PutEntityWithPassword(attempt_dbid, type_id, flags, blob, identifier,
                                       *password_hash, std::move(on_put));
    } else {
      database_->PutEntityWithOperation(operation, attempt_dbid, type_id, flags, blob, identifier,
                                        std::move(on_put));
    }
  };

  (*attempt)(dbid, kCreateDbidDuplicateRetryLimit);
}

void DBApp::OnWriteEntity(const Address& src, Channel* ch, const dbapp::WriteEntity& msg) {
  if (ch == nullptr) return;

  TimePoint t0 = Clock::now();
  const RequestCacheKey key{src, msg.request_id, msg.operation_id};
  DatabaseWriteOperation operation{msg.operation_id, msg.dbid, msg.flags, msg.entity_id};
  if (msg.owner.ownership_version != 0) {
    operation.expected_owner = msg.owner;
  }
  if (HasFlag(msg.flags, WriteFlags::kCreateNew)) {
    operation.create_owner =
        CheckoutInfo{src, msg.owner.app_id, msg.entity_id, 0, msg.owner.process_instance};
  }
  auto reject = [this, key, t0, &msg, ch](std::string error, DatabaseErrorKind kind) {
    dbapp::WriteEntityAck ack;
    ack.operation_id = msg.operation_id;
    ack.request_id = msg.request_id;
    ack.dbid = msg.dbid;
    ack.error_kind = kind;
    ack.error = std::move(error);
    ack.current_shard_table_version = shard_table_version_;
    write_reply_latency_.Record(Clock::now() - t0);
    (void)SendWriteAck(key, ack, ch);
  };
  if (operation.expected_owner && msg.owner.base_addr != src) {
    reject("checkout_owner_channel_mismatch", DatabaseErrorKind::kOwnershipConflict);
    return;
  }
  if (!operation.IsValid()) {
    reject("missing_operation_id", DatabaseErrorKind::kOperationConflict);
    return;
  }
  if (msg.owner.process_instance == 0 || msg.owner.process_instance > (UINT64_MAX >> 1)) {
    reject("invalid_checkout_process_instance", DatabaseErrorKind::kOwnershipConflict);
    return;
  }
  if (!entity_defs_ || !std::ranges::equal(msg.entity_def_digest, entity_defs_->Digest())) {
    reject("entity_def_mismatch", DatabaseErrorKind::kOperationConflict);
    return;
  }
  if (auto decoded = DecodeDbBlob(msg.blob); !decoded) {
    reject(std::format("invalid_entity_snapshot: {}", decoded.Error().Message()),
           DatabaseErrorKind::kOperationConflict);
    return;
  }
  auto hash = HashWriteOperation(operation, msg.type_id, msg.blob, msg.identifier);
  if (!hash) {
    reject(std::string(hash.Error().Message()), DatabaseErrorKind::kNone);
    return;
  }
  if (auto pending = pending_write_requests_.find(key); pending != pending_write_requests_.end()) {
    if (pending->second != *hash)
      reject("operation_id_reused", DatabaseErrorKind::kOperationConflict);
    return;
  }

  auto send_ack = [this, key, t0, dbid = msg.dbid, type_id = msg.type_id, owner = msg.owner,
                   logoff = HasFlag(msg.flags, WriteFlags::kLogOff)](PutResult result) {
    if (result.success && logoff) {
      (void)checkout_mgr_.CheckinIfOwner(dbid, type_id, owner);
    }
    dbapp::WriteEntityAck ack;
    ack.operation_id = key.operation_id;
    ack.request_id = key.request_id;
    ack.success = result.success;
    ack.error_kind = result.error_kind;
    ack.dbid = result.dbid;
    if (result.success && result.checkout_owner) {
      ack.owner = *result.checkout_owner;
    }
    ack.current_shard_table_version = shard_table_version_;
    ack.error = std::move(result.error);
    pending_write_requests_.erase(key);
    write_reply_latency_.Record(Clock::now() - t0);
    (void)SendWriteAck(key, ack, nullptr);
  };

  if (!AcceptsShardTableVersion(msg.shard_table_version)) {
    dbapp::WriteEntityAck ack;
    ack.operation_id = msg.operation_id;
    ack.request_id = msg.request_id;
    ack.success = false;
    ack.dbid = msg.dbid;
    ack.current_shard_table_version = shard_table_version_;
    ack.error = std::string(dbapp::kInvalidShardTableError);
    write_reply_latency_.Record(Clock::now() - t0);
    (void)SendWriteAck(key, ack, ch);
    return;
  }

  DatabaseID write_dbid = msg.dbid;
  WriteFlags write_flags = msg.flags;
  std::optional<dbappmgr::ShardEntry> create_shard;
  if (HasFlag(write_flags, WriteFlags::kCreateNew) &&
      !HasFlag(write_flags, WriteFlags::kExplicitDbid) && write_dbid == kInvalidDBID &&
      HasAuthoritativeShardTable()) {
    write_dbid = AllocateCreateDbidFromOwnedShard();
    if (write_dbid == kInvalidDBID) {
      send_ack(PutResult{false, kInvalidDBID, "no_dbid_range"});
      return;
    }
    write_flags = write_flags | WriteFlags::kExplicitDbid;
    if (const auto* shard = FindShard(write_dbid); shard != nullptr) create_shard = *shard;
  }

  pending_write_requests_.emplace(key, *hash);
  if (create_shard.has_value()) {
    PutCreateEntityWithRetry(*create_shard, write_dbid, msg.type_id, write_flags, msg.blob,
                             msg.identifier, std::nullopt, std::move(send_ack), operation);
  } else {
    database_->PutEntityWithOperation(operation, write_dbid, msg.type_id, write_flags, msg.blob,
                                      msg.identifier, std::move(send_ack));
  }
}

void DBApp::OnCheckoutEntity(const Address& src, Channel* ch, const dbapp::CheckoutEntity& msg) {
  if (ch == nullptr) return;

  DatabaseCheckoutOperation operation;
  operation.id = msg.operation_id;
  operation.dbid = msg.mode == dbapp::LoadMode::kByName ? kInvalidDBID : msg.dbid;
  operation.type_id = msg.type_id;
  operation.identifier = msg.identifier;
  operation.owner =
      CheckoutInfo{src, msg.owner_app_id, msg.entity_id, 0, msg.owner_process_instance};
  const bool schema_mismatch =
      !entity_defs_ || !std::ranges::equal(msg.entity_def_digest, entity_defs_->Digest());
  if (schema_mismatch || !operation.IsValid() || msg.owner_process_instance == 0 ||
      msg.owner_process_instance > (UINT64_MAX >> 1) ||
      (msg.mode != dbapp::LoadMode::kByName && msg.mode != dbapp::LoadMode::kByDbid) ||
      (msg.mode == dbapp::LoadMode::kByDbid && msg.dbid == kInvalidDBID)) {
    dbapp::CheckoutEntityAck ack;
    ack.operation_id = msg.operation_id;
    ack.request_id = msg.request_id;
    ack.status = dbapp::CheckoutStatus::kDbError;
    ack.error = schema_mismatch ? "entity_def_mismatch" : "invalid_checkout_operation";
    ack.error_kind = DatabaseErrorKind::kOperationConflict;
    (void)SendCheckoutAck({src, msg.request_id, msg.operation_id}, ack, ch);
    return;
  }

  ATLAS_LOG_DEBUG("DBApp: checkout request_id={} dbid={} type_id={} from {}:{}", msg.request_id,
                  msg.dbid, msg.type_id, src.Ip(), src.Port());

  TimePoint t0 = Clock::now();
  const RequestCacheKey key{src, msg.request_id, msg.operation_id};
  if (auto cached = checkout_ack_cache_.find(key); cached != checkout_ack_cache_.end()) {
    const auto& request = cached->second.request;
    if (!SameCheckoutRequest(request, msg)) {
      dbapp::CheckoutEntityAck ack;
      ack.operation_id = msg.operation_id;
      ack.request_id = msg.request_id;
      ack.status = dbapp::CheckoutStatus::kDbError;
      ack.error = "checkout_request_conflict";
      ack.error_kind = DatabaseErrorKind::kOperationConflict;
      (void)SendCheckoutAck(key, ack, ch);
      return;
    }
    if (cached->second.ack.status == dbapp::CheckoutStatus::kSuccess) {
      auto ack = cached->second.ack;
      database_->GetEntity(
          ack.dbid, msg.type_id, [this, key, t0, ack = std::move(ack)](GetResult result) mutable {
            const auto& held = result.checked_out_by;
            if (!result.success || !held || held->ownership_version != ack.checkout_version ||
                held->base_addr != ack.holder_addr || held->app_id != ack.holder_app_id ||
                held->entity_id != ack.holder_entity_id ||
                held->process_instance != ack.holder_process_instance) {
              ack.status = dbapp::CheckoutStatus::kDbError;
              ack.blob.clear();
              ack.error_kind = !result.success && result.error_kind != DatabaseErrorKind::kNotFound
                                   ? DatabaseErrorKind::kCommitUnknown
                                   : DatabaseErrorKind::kOwnershipConflict;
              ack.error = result.error.empty() ? "checkout_lease_expired" : std::move(result.error);
            }
            checkout_reply_latency_.Record(Clock::now() - t0);
            (void)SendCheckoutAck(key, ack, nullptr);
          });
      return;
    }
    checkout_reply_latency_.Record(Clock::now() - t0);
    (void)SendCheckoutAck(key, cached->second.ack, ch);
    return;
  }

  CheckoutInfo owner;
  owner.base_addr = src;
  owner.app_id = msg.owner_app_id;
  owner.entity_id = msg.entity_id;
  owner.process_instance = msg.owner_process_instance;

  auto send_ack = [this, key, t0, owner, msg](GetResult result) {
    dbapp::CheckoutEntityAck ack;
    ack.operation_id = msg.operation_id;
    ack.request_id = key.request_id;
    ack.dbid = result.data.dbid;
    ack.current_shard_table_version = shard_table_version_;
    ack.error_kind = result.error_kind;

    if (!result.success) {
      ack.status = result.error_kind == DatabaseErrorKind::kNotFound
                       ? dbapp::CheckoutStatus::kNotFound
                       : dbapp::CheckoutStatus::kDbError;
      ack.error = std::move(result.error);
    } else if (result.checked_out_by.has_value()) {
      ack.status = dbapp::CheckoutStatus::kAlreadyCheckedOut;
      ack.holder_addr = result.checked_out_by->base_addr;
      ack.holder_app_id = result.checked_out_by->app_id;
      ack.holder_entity_id = result.checked_out_by->entity_id;
      ack.holder_process_instance = result.checked_out_by->process_instance;
    } else {
      ack.status = dbapp::CheckoutStatus::kSuccess;
      ack.checkout_version = result.checkout_version;
      ack.holder_addr = owner.base_addr;
      ack.holder_app_id = owner.app_id;
      ack.holder_entity_id = owner.entity_id;
      ack.holder_process_instance = owner.process_instance;
      ack.blob = std::move(result.data.blob);
    }
    if (ack.error_kind != DatabaseErrorKind::kCommitUnknown &&
        (!database_->SupportsMultiDbapp() || ack.status == dbapp::CheckoutStatus::kSuccess)) {
      RememberCheckoutAck(key, msg, ack);
    }
    checkout_reply_latency_.Record(Clock::now() - t0);
    (void)SendCheckoutAck(key, ack, nullptr);
  };

  if (!AcceptsShardTableVersion(msg.shard_table_version)) {
    dbapp::CheckoutEntityAck ack;
    ack.operation_id = msg.operation_id;
    ack.request_id = msg.request_id;
    ack.status = dbapp::CheckoutStatus::kDbError;
    ack.dbid = msg.dbid;
    ack.current_shard_table_version = shard_table_version_;
    ack.error = std::string(dbapp::kInvalidShardTableError);
    checkout_reply_latency_.Record(Clock::now() - t0);
    (void)SendCheckoutAck(key, ack, ch);
    return;
  }

  if (auto pending = pending_checkout_requests_.find(key);
      pending != pending_checkout_requests_.end()) {
    if (!SameCheckoutRequest(pending->second.request, msg)) {
      dbapp::CheckoutEntityAck ack;
      ack.operation_id = msg.operation_id;
      ack.request_id = msg.request_id;
      ack.status = dbapp::CheckoutStatus::kDbError;
      ack.error = "checkout_request_conflict";
      ack.error_kind = DatabaseErrorKind::kOperationConflict;
      (void)SendCheckoutAck(key, ack, ch);
      return;
    }
    ATLAS_LOG_DEBUG("DBApp: duplicate in-flight CheckoutEntity request_id={} from {}",
                    msg.request_id, src.ToString());
    return;
  }

  auto co_result = checkout_mgr_.TryCheckout(
      msg.mode == dbapp::LoadMode::kByDbid ? msg.dbid : kInvalidDBID, msg.type_id, owner);

  const auto& held = co_result.current_owner;
  const bool same_holder = held.base_addr == owner.base_addr && held.app_id == owner.app_id &&
                           held.entity_id == owner.entity_id &&
                           held.process_instance == owner.process_instance;
  if (!same_holder && ((co_result.status == CheckoutManager::CheckoutStatus::kAlreadyCheckedOut &&
                        !database_->SupportsMultiDbapp()) ||
                       co_result.status == CheckoutManager::CheckoutStatus::kPendingCheckout)) {
    ATLAS_LOG_DEBUG(
        "DBApp: checkout conflict request_id={} dbid={} type_id={} owner={}:{} app_id={} "
        "entity_id={} state={}",
        msg.request_id, msg.dbid, msg.type_id, co_result.current_owner.base_addr.Ip(),
        co_result.current_owner.base_addr.Port(), co_result.current_owner.app_id,
        co_result.current_owner.entity_id,
        co_result.status == CheckoutManager::CheckoutStatus::kAlreadyCheckedOut ? "confirmed"
                                                                                : "pending");
    dbapp::CheckoutEntityAck ack;
    ack.operation_id = msg.operation_id;
    ack.request_id = msg.request_id;
    ack.status = dbapp::CheckoutStatus::kAlreadyCheckedOut;
    ack.dbid = msg.dbid;
    ack.current_shard_table_version = shard_table_version_;
    ack.holder_addr = co_result.current_owner.base_addr;
    ack.holder_app_id = co_result.current_owner.app_id;
    ack.holder_entity_id = co_result.current_owner.entity_id;
    ack.holder_process_instance = co_result.current_owner.process_instance;
    if (ack.error_kind != DatabaseErrorKind::kCommitUnknown &&
        (!database_->SupportsMultiDbapp() || ack.status == dbapp::CheckoutStatus::kSuccess)) {
      RememberCheckoutAck(key, msg, ack);
    }
    checkout_reply_latency_.Record(Clock::now() - t0);
    (void)SendCheckoutAck(key, ack, ch);
    return;
  }

  auto dbid = msg.dbid;
  auto type_id = msg.type_id;
  pending_checkout_requests_[key] = PendingCheckoutRequest{msg, false};

  auto on_db_done = [this, key, dbid, type_id, owner, msg,
                     by_name = msg.mode == dbapp::LoadMode::kByName,
                     send_ack = std::move(send_ack)](GetResult result) mutable {
    bool canceled = false;
    bool retired = false;
    DatabaseID cleanup_dbid = dbid;
    if (auto pending_it = pending_checkout_requests_.find(key);
        pending_it != pending_checkout_requests_.end()) {
      canceled = pending_it->second.canceled;
      retired = pending_it->second.retired;
      if (pending_it->second.request.dbid != kInvalidDBID) {
        cleanup_dbid = pending_it->second.request.dbid;
      }
      pending_checkout_requests_.erase(pending_it);
    }

    if (result.success && result.data.dbid != kInvalidDBID) {
      cleanup_dbid = result.data.dbid;
    }
    if (result.success && !result.checked_out_by &&
        (result.data.dbid == kInvalidDBID || result.checkout_version == 0)) {
      result.success = false;
      result.error = "invalid_checkout_lease";
      result.error_kind = DatabaseErrorKind::kCommitUnknown;
    }

    if (retired) {
      (void)checkout_mgr_.CheckinIfOwner(by_name ? kInvalidDBID : dbid, type_id, owner);
      if (result.success && !result.checked_out_by && result.checkout_version != 0) {
        auto acquired = owner;
        acquired.ownership_version = result.checkout_version;
        database_->ReleaseCheckout(cleanup_dbid, type_id, acquired, [](Result<void>) {});
      }
      return;
    }
    if (by_name) {
      (void)checkout_mgr_.CheckinIfOwner(kInvalidDBID, type_id, owner);
      if (result.success && !result.checked_out_by) {
        (void)checkout_mgr_.TryCheckout(cleanup_dbid, type_id, owner);
      }
    }

    if (result.success && !result.checked_out_by) {
      if (auto current = checkout_mgr_.GetOwner(result.data.dbid, type_id);
          current && current->ownership_version != 0 &&
          current->ownership_version < result.checkout_version) {
        (void)checkout_mgr_.CheckinIfOwner(result.data.dbid, type_id, *current);
        (void)checkout_mgr_.TryCheckout(result.data.dbid, type_id, owner);
      }
      (void)checkout_mgr_.ConfirmCheckoutIfOwner(result.data.dbid, type_id, owner,
                                                 result.checkout_version);
    }
    if (!result.success || result.checked_out_by) {
      (void)checkout_mgr_.CheckinIfOwner(cleanup_dbid, type_id, owner);
    }
    if (canceled) return;
    send_ack(std::move(result));
  };

  database_->CheckoutWithOperation(operation, std::move(on_db_done));
}

void DBApp::OnAbortCheckout(const Address& src, Channel* ch, const dbapp::AbortCheckout& msg) {
  if (ch == nullptr) return;
  ++abort_checkout_total_;
  const RequestCacheKey key{src, msg.request_id, msg.operation_id};
  dbapp::CheckoutEntity request;
  request.operation_id = msg.operation_id;
  request.request_id = msg.request_id;
  request.dbid = msg.dbid;
  request.type_id = msg.type_id;
  request.mode = msg.mode;
  request.identifier = msg.identifier;
  request.entity_id = msg.entity_id;
  request.owner_app_id = msg.owner_app_id;
  request.owner_process_instance = msg.owner_process_instance;
  DatabaseCheckoutOperation operation;
  operation.id = msg.operation_id;
  operation.dbid = msg.mode == dbapp::LoadMode::kByName ? kInvalidDBID : msg.dbid;
  operation.type_id = msg.type_id;
  operation.identifier = msg.identifier;
  operation.owner =
      CheckoutInfo{src, msg.owner_app_id, msg.entity_id, 0, msg.owner_process_instance};
  auto reply = [this, key](bool success) {
    dbapp::AbortCheckoutAck ack;
    ack.operation_id = key.operation_id;
    ack.request_id = key.request_id;
    ack.success = success;
    if (auto* channel = ResolveReplyChannel(key.reply_addr)) (void)channel->SendMessage(ack);
  };
  if (!operation.IsValid() || msg.owner_process_instance == 0 ||
      msg.owner_process_instance > (UINT64_MAX >> 1) ||
      (msg.mode != dbapp::LoadMode::kByDbid && msg.mode != dbapp::LoadMode::kByName) ||
      (msg.mode == dbapp::LoadMode::kByDbid && msg.dbid == kInvalidDBID)) {
    reply(false);
    return;
  }
  auto pending = pending_checkout_requests_.find(key);
  if (pending != pending_checkout_requests_.end()) {
    if (!SameCheckoutRequest(pending->second.request, request)) {
      reply(false);
      return;
    }
    ++abort_checkout_pending_hit_total_;
    if (pending->second.canceled) return;
    pending->second.canceled = true;
  } else {
    ++abort_checkout_late_hit_total_;
  }
  database_->ResolveCheckoutOperation(
      operation, [this, operation, reply = std::move(reply)](GetResult result) mutable {
        auto owner = operation.owner;
        owner.ownership_version = result.checkout_version;
        const auto dbid = result.data.dbid;
        auto finish = [this, dbid, type_id = operation.type_id, owner,
                       reply = std::move(reply)](Result<void> released) mutable {
          if (released) (void)checkout_mgr_.CheckinIfOwner(dbid, type_id, owner);
          reply(released.HasValue());
        };
        if (result.success && dbid != kInvalidDBID && owner.ownership_version != 0 &&
            !result.checked_out_by) {
          database_->ReleaseCheckout(dbid, operation.type_id, owner, std::move(finish));
        } else if ((result.error_kind == DatabaseErrorKind::kOwnershipConflict ||
                    result.error_kind == DatabaseErrorKind::kNotFound) &&
                   dbid != kInvalidDBID && owner.ownership_version != 0) {
          finish(Result<void>{});
        } else {
          finish(Error{ErrorCode::kCommitUnknown, "checkout cancellation is not confirmed"});
        }
      });
}

void DBApp::OnCheckinEntity(const Address& src, Channel* /*ch*/, const dbapp::CheckinEntity& msg) {
  auto reply = [this, src, msg](bool success, std::string error) {
    if (success) (void)checkout_mgr_.CheckinIfOwner(msg.dbid, msg.type_id, msg.owner);
    if (auto* channel = ResolveReplyChannel(src))
      (void)channel->SendMessage(dbapp::CheckinEntityAck{msg, success, std::move(error)});
  };
  if (msg.owner.base_addr != src || msg.owner.ownership_version == 0 ||
      msg.owner.process_instance == 0 || msg.dbid <= 0 || msg.type_id == 0) {
    reply(false, "invalid_checkout_owner");
    return;
  }
  database_->ReleaseCheckout(
      msg.dbid, msg.type_id, msg.owner,
      [this, msg, reply = std::move(reply)](Result<void> result) mutable {
        if (result) {
          reply(true, {});
          return;
        }
        database_->GetEntity(
            msg.dbid, msg.type_id, [msg, reply = std::move(reply)](GetResult current) mutable {
              const bool inactive =
                  current.success
                      ? !current.checked_out_by ||
                            current.checked_out_by->ownership_version > msg.owner.ownership_version
                      : current.error_kind == DatabaseErrorKind::kNotFound;
              reply(inactive, inactive ? "" : "checkout_release_unconfirmed");
            });
      });
}

void DBApp::OnDeleteEntity(const Address& src, Channel* ch, const dbapp::DeleteEntity& msg) {
  if (ch == nullptr) return;
  DatabaseWriteOperation operation{msg.operation_id, msg.dbid, WriteFlags::kDelete,
                                   msg.owner.entity_id, msg.owner};
  if (!operation.IsValid() || msg.owner.ownership_version == 0 || msg.owner.base_addr != src) {
    dbapp::DeleteEntityAck ack;
    ack.request_id = msg.request_id;
    ack.error = "missing_or_invalid_checkout_owner";
    (void)ch->SendMessage(ack);
    return;
  }
  database_->PutEntityWithOperation(operation, msg.dbid, msg.type_id, WriteFlags::kDelete, {}, "",
                                    [this, src, msg](PutResult result) {
                                      if (result.success)
                                        (void)checkout_mgr_.CheckinIfOwner(msg.dbid, msg.type_id,
                                                                           msg.owner);
                                      dbapp::DeleteEntityAck ack;
                                      ack.request_id = msg.request_id;
                                      ack.success = result.success;
                                      ack.error = std::move(result.error);
                                      if (auto* reply = ResolveReplyChannel(src))
                                        (void)reply->SendMessage(ack);
                                    });
}

void DBApp::OnLookupEntity(const Address& src, Channel* ch, const dbapp::LookupEntity& msg) {
  if (ch == nullptr) return;

  database_->LookupByName(
      msg.type_id, msg.identifier,
      [this, reply_addr = src, request_id = msg.request_id](LookupResult result) {
        dbapp::LookupEntityAck ack;
        ack.request_id = request_id;
        ack.found = result.found;
        ack.dbid = result.dbid;
        if (auto* reply_ch = this->ResolveReplyChannel(reply_addr)) {
          (void)reply_ch->SendMessage(ack);
        }
      });
}

void DBApp::OnGetEntityIds(const Address& src, Channel* ch, const dbapp::GetEntityIds& msg) {
  if (ch == nullptr) return;

  if (!id_allocator_) {
    ATLAS_LOG_ERROR("DBApp: GetEntityIds but allocator not ready");
    return;
  }

  id_allocator_->Allocate(msg.count, [this, reply_addr = src](Result<EntityIdRange> range) {
    dbapp::GetEntityIdsAck ack;
    if (range) {
      ack.start = range->first;
      ack.end = range->last;
      ack.count = range->last - range->first + 1;
    } else {
      ATLAS_LOG_ERROR("DBApp: EntityID reservation failed: {}", range.Error().Message());
    }
    if (auto* reply_ch = ResolveReplyChannel(reply_addr)) {
      if (auto sent = reply_ch->SendMessage(ack); !sent) {
        ATLAS_LOG_WARNING("DBApp: EntityID reply send failed: {}", sent.Error().Message());
      }
    }
  });
}

void DBApp::OnPutEntityIds(const Address& src, Channel* ch, const dbapp::PutEntityIds& msg) {
  if (ch == nullptr) return;

  ATLAS_LOG_DEBUG("DBApp: PutEntityIds [{}, {}] from {}:{} (not recycled)", msg.start, msg.end,
                  src.Ip(), src.Port());

  dbapp::PutEntityIdsAck ack;
  ack.success = true;
  if (auto* reply_ch = ResolveReplyChannel(src)) {
    (void)reply_ch->SendMessage(ack);
  }
}

void DBApp::OnBaseappDeath(const Address& internal_addr, uint64_t process_instance) {
  if (process_instance == 0 || process_instance > (UINT64_MAX >> 1)) {
    ATLAS_LOG_ERROR("DBApp: cannot retire BaseApp without a valid process instance");
    return;
  }
  for (auto& [key, pending] : pending_checkout_requests_) {
    if (key.reply_addr == internal_addr &&
        pending.request.owner_process_instance == process_instance)
      pending.retired = true;
  }
  std::erase_if(checkout_ack_cache_, [&](const auto& entry) {
    return entry.first.reply_addr == internal_addr &&
           entry.second.request.owner_process_instance == process_instance;
  });
  for (const auto& process : process_retirements_) {
    if (process.addr == internal_addr && process.instance == process_instance) return;
  }
  process_retirements_.push_back({internal_addr, process_instance});
  RetireBaseappProcess(internal_addr, process_instance);
}

void DBApp::RetireBaseappProcess(const Address& internal_addr, uint64_t process_instance) {
  auto find = [&] {
    return std::find_if(
        process_retirements_.begin(), process_retirements_.end(),
        [&](const auto& p) { return p.addr == internal_addr && p.instance == process_instance; });
  };
  auto pending = find();
  if (pending == process_retirements_.end() || pending->in_flight) return;
  pending->in_flight = true;
  database_->RetireCheckoutProcess(
      internal_addr, process_instance, [this, internal_addr, process_instance](Result<int> result) {
        auto pending = std::find_if(
            process_retirements_.begin(), process_retirements_.end(), [&](const auto& p) {
              return p.addr == internal_addr && p.instance == process_instance;
            });
        if (pending == process_retirements_.end()) return;
        if (!result) {
          pending->in_flight = false;
          pending->next_attempt = Clock::now() + std::chrono::seconds(1);
          ATLAS_LOG_ERROR("DBApp: checkout process retirement unconfirmed: {}",
                          result.Error().Message());
          return;
        }
        checkout_mgr_.ClearAllFor(internal_addr, process_instance);
        process_retirements_.erase(pending);
        ATLAS_LOG_INFO("DBApp: retired BaseApp process instance; cleared {} DB checkouts", *result);
      });
}

void DBApp::OnAuthLogin(const Address& src, Channel* ch, const login::AuthLogin& msg) {
  ATLAS_LOG_DEBUG("DBApp: auth login request_id={} user='{}' from {}:{}", msg.request_id,
                  msg.username, src.Ip(), src.Port());
  if (!entity_defs_ || !std::ranges::equal(msg.entity_def_digest, entity_defs_->Digest())) {
    login::AuthLoginResult reply;
    reply.request_id = msg.request_id;
    reply.status = login::LoginStatus::kDefMismatch;
    if (ch != nullptr) (void)ch->SendMessage(reply);
    return;
  }
  if (authentication_stopping_ || IsShuttingDown()) {
    login::AuthLoginResult reply;
    reply.request_id = msg.request_id;
    reply.status = login::LoginStatus::kServerNotReady;
    SendAuthenticationResult(src, reply);
    return;
  }
  if (!database_ || !login::AreCredentialsValid(msg.username, msg.password_hash)) {
    login::AuthLoginResult reply;
    reply.request_id = msg.request_id;
    reply.success = false;
    reply.status =
        database_ ? login::LoginStatus::kInvalidCredentials : login::LoginStatus::kInternalError;
    if (auto* reply_ch = this->ResolveReplyChannel(src)) {
      (void)reply_ch->SendMessage(reply);
    }
    return;
  }

  if (pending_authentications_.size() >= kMaxQueuedAuthentications) {
    login::AuthLoginResult reply;
    reply.request_id = msg.request_id;
    reply.status = login::LoginStatus::kServerBusy;
    SendAuthenticationResult(src, reply);
    return;
  }
  pending_authentications_.push_back(
      {src, msg.request_id, [this, src, msg] { StartAuthentication(src, msg); }});
  PumpAuthentication();
}

void DBApp::SendAuthenticationResult(const Address& src, const login::AuthLoginResult& reply) {
  if (auto* channel = ResolveReplyChannel(src)) (void)channel->SendMessage(reply);
}

void DBApp::PumpAuthentication() {
  while (!authentication_stopping_ && active_authentications_ < kMaxActiveAuthentications &&
         !pending_authentications_.empty()) {
    auto pending = std::move(pending_authentications_.front());
    pending_authentications_.pop_front();
    ++active_authentications_;
    pending.start();
  }
}

void DBApp::StopAuthentication() {
  authentication_stopping_ = true;
  if (authentication_tasks_) authentication_tasks_->StopAccepting();
  while (!pending_authentications_.empty()) {
    auto pending = std::move(pending_authentications_.front());
    pending_authentications_.pop_front();
    login::AuthLoginResult reply;
    reply.request_id = pending.request_id;
    reply.status = login::LoginStatus::kServerNotReady;
    SendAuthenticationResult(pending.reply_addr, reply);
  }
}

void DBApp::StartAuthentication(const Address& src, const login::AuthLogin& msg) {
  if (!authentication_tasks_)
    authentication_tasks_ =
        std::make_unique<BgTaskManager>(Dispatcher(), kMaxActiveAuthentications);
  auto finished = std::make_shared<bool>(false);
  auto complete = [this, src, finished](const login::AuthLoginResult& reply) {
    if (std::exchange(*finished, true)) return;
    --active_authentications_;
    SendAuthenticationResult(src, reply);
    PumpAuthentication();
  };
  database_->LookupByName(
      account_type_id_, msg.username,
      [this, request_id = msg.request_id, password_hash = msg.password_hash,
       auto_create = msg.auto_create, username = msg.username, complete](LookupResult result) {
        login::AuthLoginResult reply;
        reply.request_id = request_id;
        reply.status = login::LoginStatus::kInternalError;
        if (authentication_stopping_) {
          reply.status = login::LoginStatus::kServerNotReady;
          complete(reply);
          return;
        }
        auto complete_lookup = [this, request_id, password_hash, complete,
                                type_id = account_type_id_](LookupResult account) {
          login::AuthLoginResult response;
          response.request_id = request_id;
          response.status = login::LoginStatus::kInternalError;
          if (authentication_stopping_) {
            response.status = login::LoginStatus::kServerNotReady;
            complete(response);
            return;
          }
          if (!account.error.empty() || !account.found || account.dbid == kInvalidDBID) {
            complete(response);
            return;
          }
          auto verified = std::make_shared<Result<bool>>(Error{ErrorCode::kInternalError});
          auto submitted = authentication_tasks_->AddTask(
              [password_hash, record = std::move(account.password_hash),
               verified]() -> Result<void> {
                *verified = VerifyPassword(password_hash, record);
                return {};
              },
              [this, complete, response, verified, dbid = account.dbid,
               type_id](Result<void> task) mutable {
                if (authentication_stopping_) {
                  response.status = login::LoginStatus::kServerNotReady;
                } else if (task && verified->HasValue() && **verified) {
                  response.success = true;
                  response.status = login::LoginStatus::kSuccess;
                  response.dbid = dbid;
                  response.type_id = type_id;
                } else if (task && (verified->HasValue() ||
                                    verified->Error().Code() == ErrorCode::kInvalidArgument)) {
                  response.status = login::LoginStatus::kInvalidCredentials;
                }
                complete(response);
              });
          if (!submitted) complete(response);
        };
        if (!result.error.empty()) {
          ATLAS_LOG_ERROR("DBApp: auth lookup failed for '{}': {}", username, result.error);
          complete(reply);
          return;
        }
        if (result.found) {
          complete_lookup(std::move(result));
          return;
        }
        if (!auto_create || !auto_create_accounts_) {
          reply.status = login::LoginStatus::kInvalidCredentials;
          complete(reply);
          return;
        }
        auto hashed = std::make_shared<Result<std::string>>(Error{ErrorCode::kInternalError});
        auto submitted = authentication_tasks_->AddTask(
            [password_hash, hashed]() -> Result<void> {
              *hashed = HashPassword(password_hash);
              return {};
            },
            [this, username, request_id, complete_lookup, complete, reply,
             hashed](Result<void> task) mutable {
              if (authentication_stopping_) {
                reply.status = login::LoginStatus::kServerNotReady;
                complete(reply);
                return;
              }
              if (!task || !hashed->HasValue()) {
                complete(reply);
                return;
              }
              EntityData data;
              data.type_id = account_type_id_;
              data.identifier = username;
              auto encoded = EncodeDbBlob({}, {});
              if (!encoded) {
                ATLAS_LOG_ERROR("DBApp: cannot encode initial account snapshot: {}",
                                encoded.Error().Message());
                reply.success = false;
                reply.status = login::LoginStatus::kInternalError;
                complete(reply);
                return;
              }
              data.blob = std::move(*encoded);
              DatabaseID create_dbid = kInvalidDBID;
              WriteFlags create_flags = WriteFlags::kCreateNew;
              std::optional<dbappmgr::ShardEntry> create_shard;
              if (HasAuthoritativeShardTable()) {
                create_dbid = AllocateCreateDbidForRoute(DbShardRouteKey(username));
                if (create_dbid == kInvalidDBID) {
                  ATLAS_LOG_ERROR("DBApp: no owned shard range for account '{}'", username);
                  reply.success = false;
                  reply.status = login::LoginStatus::kInternalError;
                  complete(reply);
                  return;
                }
                create_flags = create_flags | WriteFlags::kExplicitDbid;
                if (const auto* shard = FindShard(create_dbid); shard != nullptr) {
                  create_shard = *shard;
                }
              }
              auto on_put = [this, request_id, username, complete_lookup, complete,
                             type_id = account_type_id_](PutResult put) {
                if (!put.success && (put.error_kind == DatabaseErrorKind::kDuplicateIdentifier ||
                                     put.error_kind == DatabaseErrorKind::kCommitUnknown)) {
                  database_->LookupByName(type_id, username, complete_lookup);
                  return;
                }
                login::AuthLoginResult r;
                r.request_id = request_id;
                if (put.success) {
                  r.success = true;
                  r.status = login::LoginStatus::kSuccess;
                  r.dbid = put.dbid;
                  r.type_id = type_id;
                } else {
                  r.success = false;
                  r.status = login::LoginStatus::kInternalError;
                }
                complete(r);
              };
              if (create_shard.has_value()) {
                PutCreateEntityWithRetry(*create_shard, create_dbid, account_type_id_, create_flags,
                                         std::move(data.blob), std::move(data.identifier), **hashed,
                                         std::move(on_put));
              } else {
                database_->PutEntityWithPassword(create_dbid, account_type_id_, create_flags,
                                                 data.blob, data.identifier, **hashed,
                                                 std::move(on_put));
              }
            });
        if (!submitted) complete(reply);
      });
}

}  // namespace atlas
