#include "server/server_config.h"

#include <algorithm>
#include <format>
#include <optional>
#include <string>
#include <system_error>
#include <type_traits>
#include <variant>

#include "network/network_interface.h"
#include "platform/environment.h"
#include "serialization/data_section.h"

namespace atlas {

namespace {

auto ValidateDatabaseType(std::string_view type) -> Result<void> {
  if (type == "sqlite") {
    return Error{ErrorCode::kNotSupported,
                 "SQLite entity persistence is no longer supported; use Oracle MySQL 8.4"};
  }
  if (type != "mysql") {
    return Error{ErrorCode::kInvalidArgument,
                 std::format("unsupported database backend '{}'; use mysql", type)};
  }
  return {};
}

auto ParseAddress(std::string_view s) -> Result<Address> {
  auto colon = s.rfind(':');
  if (colon == std::string_view::npos)
    return Error{ErrorCode::kInvalidArgument, std::format("address '{}' missing port", s)};

  auto host = s.substr(0, colon);
  auto port_str = s.substr(colon + 1);

  uint16_t port = 0;
  try {
    int p = std::stoi(std::string(port_str));
    if (p < 0 || p > 65535) throw std::out_of_range("port out of range");
    port = static_cast<uint16_t>(p);
  } catch (...) {
    return Error{ErrorCode::kInvalidArgument, std::format("invalid port in address '{}'", s)};
  }

  return Address::Resolve(host, port).AndThen([](Address a) -> Result<Address> { return a; });
}

auto ParseLogLevel(std::string_view s) -> LogLevel {
  auto lower = std::string(s);
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

  if (lower == "trace") return LogLevel::kTrace;
  if (lower == "debug") return LogLevel::kDebug;
  if (lower == "info") return LogLevel::kInfo;
  if (lower == "warning" || lower == "warn") return LogLevel::kWarning;
  if (lower == "error") return LogLevel::kError;
  if (lower == "critical") return LogLevel::kCritical;
  if (lower == "off") return LogLevel::kOff;
  return LogLevel::kInfo;
}

auto ParseBoolString(std::string_view s) -> bool {
  auto lower = std::string(s);
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return lower == "1" || lower == "true" || lower == "yes" || lower == "on";
}

auto ValidProductionValue(std::string_view value) -> bool {
  auto lower = std::string(value);
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return lower == "true" || lower == "false" || lower == "1" || lower == "0" || lower == "yes" ||
         lower == "no" || lower == "on" || lower == "off";
}

void LoadStringArray(const DataSection* section, std::vector<std::string>& out) {
  if (!section) {
    return;
  }

  out.clear();
  for (auto* child : section->Children()) {
    out.emplace_back(child->Value());
  }
}

using FieldPtr =
    std::variant<std::string ServerConfig::*, int ServerConfig::*, uint16_t ServerConfig::*,
                 bool ServerConfig::*, std::filesystem::path ServerConfig::*>;

struct CliField {
  std::string_view key;
  FieldPtr ptr;
};

// clang-format off
static const CliField kCliFields[] = {
    {"name",                       &ServerConfig::process_name},
    {"internal-port",              &ServerConfig::internal_port},
    {"external-port",              &ServerConfig::external_port},
    {"external-dtls-certificate",  &ServerConfig::external_dtls_certificate},
    {"external-dtls-private-key",  &ServerConfig::external_dtls_private_key},
    {"production",                 &ServerConfig::is_production},
    {"update-hertz",               &ServerConfig::update_hertz},
    {"assembly",                   &ServerConfig::script_assembly},
    {"runtime-config",             &ServerConfig::runtime_config},
    {"entitydef-bin-path",         &ServerConfig::entitydef_bin_path},
    {"db-type",                    &ServerConfig::db_type},
    {"db-mysql-host",              &ServerConfig::db_mysql_host},
    {"db-mysql-port",              &ServerConfig::db_mysql_port},
    {"db-mysql-user",              &ServerConfig::db_mysql_user},
    {"db-mysql-password",          &ServerConfig::db_mysql_password},
    {"db-mysql-database",          &ServerConfig::db_mysql_database},
    {"db-mysql-database-id",       &ServerConfig::db_mysql_database_id},
    {"db-mysql-ssl-ca",            &ServerConfig::db_mysql_ssl_ca},
    {"db-mysql-initialize",        &ServerConfig::db_mysql_initialize},
    {"db-mysql-pool-size",         &ServerConfig::db_mysql_pool_size},
    {"auto-create-accounts",       &ServerConfig::auto_create_accounts},
    {"account-type-id",            &ServerConfig::account_type_id},
    {"login-rate-limit-per-ip",    &ServerConfig::login_rate_limit_per_ip},
    {"login-rate-limit-global",    &ServerConfig::login_rate_limit_global},
    {"login-rate-limit-window-sec",&ServerConfig::login_rate_limit_window_sec},
    {"enable-hot-reload",           &ServerConfig::enable_hot_reload},
    {"hot-reload-project",          &ServerConfig::hot_reload_script_project_path},
    {"hot-reload-output-dir",       &ServerConfig::hot_reload_output_directory},
    {"hot-reload-assembly-name",    &ServerConfig::hot_reload_assembly_name},
    {"hot-reload-debounce-ms",      &ServerConfig::hot_reload_debounce_ms},
    {"hot-reload-unload-timeout-ms",&ServerConfig::hot_reload_unload_timeout_ms},
    {"hot-reload-auto-compile",     &ServerConfig::hot_reload_auto_compile},
    {"revive-cellappmgr-exe",       &ServerConfig::revive_cellappmgr_exe},
    {"revive-cellappmgr-name",      &ServerConfig::revive_cellappmgr_name},
    {"revive-cellappmgr-port",      &ServerConfig::revive_cellappmgr_internal_port},
    {"revive-cellappmgr-output-path", &ServerConfig::revive_cellappmgr_output_path},
    {"revive-cellappmgr-priority",  &ServerConfig::revive_cellappmgr_priority},
    {"revive-cellappmgr-update-hertz",  &ServerConfig::revive_cellappmgr_update_hertz},
    {"revive-cellappmgr-launch-timeout-ms",
        &ServerConfig::revive_cellappmgr_launch_timeout_ms},
    {"revive-restart-delay-ms",     &ServerConfig::revive_restart_delay_ms},
    {"revive-restart-backoff-cap-ms", &ServerConfig::revive_restart_backoff_cap_ms},
    {"revive-max-restarts",         &ServerConfig::revive_max_restarts},
    {"revive-cellappmgr-health-interval-ms",
        &ServerConfig::revive_cellappmgr_health_interval_ms},
    {"revive-cellappmgr-heartbeat-timeout-ms",
        &ServerConfig::revive_cellappmgr_heartbeat_timeout_ms},
    {"revive-cellappmgr-manager-health-timeout-ms",
        &ServerConfig::revive_cellappmgr_manager_health_timeout_ms},
    {"revive-cellappmgr-health-failure-threshold",
        &ServerConfig::revive_cellappmgr_health_failure_threshold},
    {"revive-cellappmgr-audit-interval-ms",
        &ServerConfig::revive_cellappmgr_audit_interval_ms},
    {"revive-cellappmgr-missing-audit-threshold",
        &ServerConfig::revive_cellappmgr_missing_audit_threshold},
    {"revive-cellappmgr-on-start",  &ServerConfig::revive_cellappmgr_on_start},
    {"revive-baseappmgr-exe",       &ServerConfig::revive_baseappmgr_exe},
    {"revive-baseappmgr-name",      &ServerConfig::revive_baseappmgr_name},
    {"revive-baseappmgr-port",      &ServerConfig::revive_baseappmgr_internal_port},
    {"revive-baseappmgr-output-path", &ServerConfig::revive_baseappmgr_output_path},
    {"revive-baseappmgr-priority",  &ServerConfig::revive_baseappmgr_priority},
    {"revive-baseappmgr-update-hertz",  &ServerConfig::revive_baseappmgr_update_hertz},
    {"revive-baseappmgr-launch-timeout-ms",
        &ServerConfig::revive_baseappmgr_launch_timeout_ms},
    {"revive-baseappmgr-on-start",  &ServerConfig::revive_baseappmgr_on_start},
    {"revive-dbappmgr-exe",         &ServerConfig::revive_dbappmgr_exe},
    {"revive-dbappmgr-name",        &ServerConfig::revive_dbappmgr_name},
    {"revive-dbappmgr-port",        &ServerConfig::revive_dbappmgr_internal_port},
    {"revive-dbappmgr-output-path", &ServerConfig::revive_dbappmgr_output_path},
    {"revive-dbappmgr-priority",    &ServerConfig::revive_dbappmgr_priority},
    {"revive-dbappmgr-update-hertz",&ServerConfig::revive_dbappmgr_update_hertz},
    {"revive-dbappmgr-launch-timeout-ms",
        &ServerConfig::revive_dbappmgr_launch_timeout_ms},
    {"revive-dbappmgr-on-start",    &ServerConfig::revive_dbappmgr_on_start},
    {"mesh-enabled",                &ServerConfig::mesh_enabled},
    {"mesh-advertise-ip",           &ServerConfig::mesh_advertise_ip},
    {"mesh-broadcast-ip",           &ServerConfig::mesh_broadcast_ip},
};
// clang-format on

auto ParseAndAssign(ServerConfig& cfg, const FieldPtr& ptr, std::string_view key,
                    std::string_view val) -> std::optional<Error> {
  return std::visit(
      [&](auto member) -> std::optional<Error> {
        using T = std::remove_reference_t<decltype(cfg.*member)>;

        if constexpr (std::is_same_v<T, std::string> || std::is_same_v<T, std::filesystem::path>) {
          cfg.*member = std::string(val);
        } else if constexpr (std::is_same_v<T, bool>) {
          cfg.*member = ParseBoolString(val);
        } else if constexpr (std::is_same_v<T, int>) {
          try {
            cfg.*member = std::stoi(std::string(val));
          } catch (...) {
            return Error{ErrorCode::kInvalidArgument, std::format("invalid --{}", key)};
          }
        } else if constexpr (std::is_same_v<T, uint16_t>) {
          try {
            int p = std::stoi(std::string(val));
            if (p < 0 || p > 65535) throw std::out_of_range("out of range");
            cfg.*member = static_cast<uint16_t>(p);
          } catch (...) {
            return Error{ErrorCode::kInvalidArgument, std::format("invalid --{}", key)};
          }
        }
        return std::nullopt;
      },
      ptr);
}

}  // namespace

auto ServerConfig::FromJsonFile(const std::filesystem::path& path) -> Result<ServerConfig> {
  auto tree_result = DataSectionTree::FromJson(path);
  if (!tree_result) return tree_result.Error();

  auto tree = std::move(*tree_result);
  auto* root = tree->Root();

  ServerConfig cfg;
  std::error_code ec;
  cfg.config_path = std::filesystem::absolute(path, ec);
  if (ec) cfg.config_path = path;
  cfg.raw_config = tree;

  cfg.update_hertz =
      static_cast<int>(root->ReadUint("update_hertz", static_cast<uint32_t>(cfg.update_hertz)));
  if (auto* production = root->Child("is_production");
      production && (!production->Children().empty() || !ValidProductionValue(production->Value())))
    return Error{ErrorCode::kInvalidArgument, "is_production must be a boolean"};
  if (auto* production = root->Child("is_production"))
    cfg.is_production = ParseBoolString(production->Value());
  cfg.external_dtls_certificate = root->ReadString("external_dtls_certificate", "");
  cfg.external_dtls_private_key = root->ReadString("external_dtls_private_key", "");

  auto machined_str = root->ReadString("machined_address", "");
  if (!machined_str.empty()) {
    auto addr_result = ParseAddress(machined_str);
    if (addr_result) cfg.machined_address = *addr_result;
  }

  auto log_level_str = root->ReadString("log_level", "");
  if (!log_level_str.empty()) cfg.log_level = ParseLogLevel(log_level_str);

  if (auto* script = root->Child("script")) {
    auto assembly = script->ReadString("assembly", "");
    if (!assembly.empty()) cfg.script_assembly = assembly;

    auto runtimecfg = script->ReadString("runtime_config", "");
    if (!runtimecfg.empty()) cfg.runtime_config = runtimecfg;
  }

  if (auto* hr = root->Child("hot_reload")) {
    cfg.enable_hot_reload = hr->ReadBool("enabled", cfg.enable_hot_reload);
    cfg.hot_reload_script_project_path =
        hr->ReadString("script_project_path", cfg.hot_reload_script_project_path.string());
    cfg.hot_reload_output_directory =
        hr->ReadString("output_directory", cfg.hot_reload_output_directory.string());
    cfg.hot_reload_assembly_name = hr->ReadString("assembly_name", cfg.hot_reload_assembly_name);
    cfg.hot_reload_debounce_ms = hr->ReadInt("debounce_ms", cfg.hot_reload_debounce_ms);
    cfg.hot_reload_unload_timeout_ms =
        hr->ReadInt("unload_timeout_ms", cfg.hot_reload_unload_timeout_ms);
    cfg.hot_reload_auto_compile = hr->ReadBool("auto_compile", cfg.hot_reload_auto_compile);
  }

  if (auto* reviver = root->Child("reviver")) {
    cfg.revive_restart_delay_ms = reviver->ReadInt("restart_delay_ms", cfg.revive_restart_delay_ms);
    cfg.revive_restart_backoff_cap_ms =
        reviver->ReadInt("restart_backoff_cap_ms", cfg.revive_restart_backoff_cap_ms);
    cfg.revive_max_restarts = reviver->ReadInt("max_restarts", cfg.revive_max_restarts);
    if (auto* cellappmgr = reviver->Child("cellappmgr")) {
      cfg.revive_cellappmgr_exe = cellappmgr->ReadString("exe", cfg.revive_cellappmgr_exe.string());
      cfg.revive_cellappmgr_name = cellappmgr->ReadString("name", cfg.revive_cellappmgr_name);
      cfg.revive_cellappmgr_internal_port = static_cast<uint16_t>(
          cellappmgr->ReadUint("internal_port", cfg.revive_cellappmgr_internal_port));
      cfg.revive_cellappmgr_output_path =
          cellappmgr->ReadString("output_path", cfg.revive_cellappmgr_output_path.string());
      cfg.revive_cellappmgr_priority =
          cellappmgr->ReadInt("priority", cfg.revive_cellappmgr_priority);
      cfg.revive_cellappmgr_update_hertz =
          cellappmgr->ReadInt("update_hertz", cfg.revive_cellappmgr_update_hertz);
      cfg.revive_cellappmgr_launch_timeout_ms =
          cellappmgr->ReadInt("launch_timeout_ms", cfg.revive_cellappmgr_launch_timeout_ms);
      cfg.revive_cellappmgr_health_interval_ms =
          cellappmgr->ReadInt("health_interval_ms", cfg.revive_cellappmgr_health_interval_ms);
      cfg.revive_cellappmgr_heartbeat_timeout_ms =
          cellappmgr->ReadInt("heartbeat_timeout_ms", cfg.revive_cellappmgr_heartbeat_timeout_ms);
      cfg.revive_cellappmgr_manager_health_timeout_ms = cellappmgr->ReadInt(
          "manager_health_timeout_ms", cfg.revive_cellappmgr_manager_health_timeout_ms);
      cfg.revive_cellappmgr_health_failure_threshold = cellappmgr->ReadInt(
          "health_failure_threshold", cfg.revive_cellappmgr_health_failure_threshold);
      cfg.revive_cellappmgr_audit_interval_ms =
          cellappmgr->ReadInt("audit_interval_ms", cfg.revive_cellappmgr_audit_interval_ms);
      cfg.revive_cellappmgr_missing_audit_threshold = cellappmgr->ReadInt(
          "missing_audit_threshold", cfg.revive_cellappmgr_missing_audit_threshold);
      cfg.revive_cellappmgr_on_start =
          cellappmgr->ReadBool("on_start", cfg.revive_cellappmgr_on_start);
    }
    if (auto* baseappmgr = reviver->Child("baseappmgr")) {
      cfg.revive_baseappmgr_exe = baseappmgr->ReadString("exe", cfg.revive_baseappmgr_exe.string());
      cfg.revive_baseappmgr_name = baseappmgr->ReadString("name", cfg.revive_baseappmgr_name);
      cfg.revive_baseappmgr_internal_port = static_cast<uint16_t>(
          baseappmgr->ReadUint("internal_port", cfg.revive_baseappmgr_internal_port));
      cfg.revive_baseappmgr_output_path =
          baseappmgr->ReadString("output_path", cfg.revive_baseappmgr_output_path.string());
      cfg.revive_baseappmgr_priority =
          baseappmgr->ReadInt("priority", cfg.revive_baseappmgr_priority);
      cfg.revive_baseappmgr_update_hertz =
          baseappmgr->ReadInt("update_hertz", cfg.revive_baseappmgr_update_hertz);
      cfg.revive_baseappmgr_launch_timeout_ms =
          baseappmgr->ReadInt("launch_timeout_ms", cfg.revive_baseappmgr_launch_timeout_ms);
      cfg.revive_baseappmgr_on_start =
          baseappmgr->ReadBool("on_start", cfg.revive_baseappmgr_on_start);
    }
    if (auto* dbappmgr = reviver->Child("dbappmgr")) {
      cfg.revive_dbappmgr_exe = dbappmgr->ReadString("exe", cfg.revive_dbappmgr_exe.string());
      cfg.revive_dbappmgr_name = dbappmgr->ReadString("name", cfg.revive_dbappmgr_name);
      cfg.revive_dbappmgr_internal_port = static_cast<uint16_t>(
          dbappmgr->ReadUint("internal_port", cfg.revive_dbappmgr_internal_port));
      cfg.revive_dbappmgr_output_path =
          dbappmgr->ReadString("output_path", cfg.revive_dbappmgr_output_path.string());
      cfg.revive_dbappmgr_priority = dbappmgr->ReadInt("priority", cfg.revive_dbappmgr_priority);
      cfg.revive_dbappmgr_update_hertz =
          dbappmgr->ReadInt("update_hertz", cfg.revive_dbappmgr_update_hertz);
      cfg.revive_dbappmgr_launch_timeout_ms =
          dbappmgr->ReadInt("launch_timeout_ms", cfg.revive_dbappmgr_launch_timeout_ms);
      cfg.revive_dbappmgr_on_start = dbappmgr->ReadBool("on_start", cfg.revive_dbappmgr_on_start);
    }
  }

  if (auto* db = root->Child("database")) {
    auto edef_bin = db->ReadString("entitydef_bin_path", "");
    if (!edef_bin.empty()) cfg.entitydef_bin_path = edef_bin;

    if (db->Child("type")) cfg.db_type = db->ReadString("type", "");
    if (auto valid = ValidateDatabaseType(cfg.db_type); !valid) return valid.Error();

    for (const char* key :
         {"sqlite_path", "sqlite_wal", "sqlite_busy_timeout_ms", "sqlite_foreign_keys"}) {
      if (db->Child(key)) {
        return Error{ErrorCode::kNotSupported,
                     std::format("removed SQLite entity persistence setting '{}'", key)};
      }
    }
    cfg.db_mysql_host = db->ReadString("mysql_host", cfg.db_mysql_host);
    cfg.db_mysql_port = static_cast<uint16_t>(db->ReadUint("mysql_port", cfg.db_mysql_port));
    cfg.db_mysql_user = db->ReadString("mysql_user", cfg.db_mysql_user);
    cfg.db_mysql_password = db->ReadString("mysql_password", cfg.db_mysql_password);
    cfg.db_mysql_database = db->ReadString("mysql_database", cfg.db_mysql_database);
    cfg.db_mysql_database_id = db->ReadString("mysql_database_id", cfg.db_mysql_database_id);
    cfg.db_mysql_ssl_ca = db->ReadString("mysql_ssl_ca", cfg.db_mysql_ssl_ca);
    cfg.db_mysql_initialize = db->ReadBool("mysql_initialize", cfg.db_mysql_initialize);
    cfg.db_mysql_pool_size = db->ReadInt("mysql_pool_size", cfg.db_mysql_pool_size);
  }

  cfg.auto_create_accounts = root->ReadBool("auto_create_accounts", cfg.auto_create_accounts);
  cfg.account_type_id =
      static_cast<uint16_t>(root->ReadUint("account_type_id", cfg.account_type_id));
  cfg.login_rate_limit_per_ip =
      root->ReadInt("login_rate_limit_per_ip", cfg.login_rate_limit_per_ip);
  cfg.login_rate_limit_global =
      root->ReadInt("login_rate_limit_global", cfg.login_rate_limit_global);
  cfg.login_rate_limit_window_sec =
      root->ReadInt("login_rate_limit_window_sec", cfg.login_rate_limit_window_sec);
  LoadStringArray(root->Child("login_rate_limit_trusted_cidrs"),
                  cfg.login_rate_limit_trusted_cidrs);

  if (auto* auth = root->Child("authentication")) {
    cfg.auto_create_accounts = auth->ReadBool("auto_create_accounts", cfg.auto_create_accounts);
    cfg.account_type_id =
        static_cast<uint16_t>(auth->ReadUint("account_type_id", cfg.account_type_id));

    if (auto* rate_limit = auth->Child("rate_limit")) {
      cfg.login_rate_limit_per_ip = rate_limit->ReadInt("per_ip", cfg.login_rate_limit_per_ip);
      cfg.login_rate_limit_global = rate_limit->ReadInt("global", cfg.login_rate_limit_global);
      cfg.login_rate_limit_window_sec =
          rate_limit->ReadInt("window_sec", cfg.login_rate_limit_window_sec);
      LoadStringArray(rate_limit->Child("trusted_cidrs"), cfg.login_rate_limit_trusted_cidrs);
    }
  }

  if (auto valid = ValidateDatabaseType(cfg.db_type); !valid) return valid.Error();
  return cfg;
}

auto ServerConfig::FromArgs(int argc, char* argv[]) -> Result<ServerConfig> {
  ServerConfig cfg;

  for (int i = 1; i < argc; ++i) {
    if (std::string_view(argv[i]).starts_with("--db-sqlite-")) {
      return Error{ErrorCode::kNotSupported,
                   "SQLite entity persistence is no longer supported; use Oracle MySQL 8.4"};
    }
    if (i + 1 >= argc) break;
    std::string_view key = argv[i];
    std::string_view val = argv[i + 1];

    if (key.starts_with("--"))
      key.remove_prefix(2);
    else
      continue;

    if (key == "type") {
      auto pt = ProcessTypeFromName(val);
      if (!pt) return pt.Error();
      cfg.process_type = *pt;
      ++i;
    } else if (key == "machined") {
      auto addr = ParseAddress(val);
      if (!addr) return addr.Error();
      cfg.machined_address = *addr;
      ++i;
    } else if (key == "log-level") {
      cfg.log_level = ParseLogLevel(val);
      ++i;
    } else if (key == "login-rate-limit-trusted-cidr") {
      cfg.login_rate_limit_trusted_cidrs.emplace_back(val);
      ++i;
    } else {
      for (const auto& field : kCliFields) {
        if (key == field.key) {
          if (key == "production" && !ValidProductionValue(val))
            return Error{ErrorCode::kInvalidArgument, "--production requires a boolean value"};
          auto err = ParseAndAssign(cfg, field.ptr, key, val);
          if (err) return *err;
          ++i;
          break;
        }
      }
    }
  }

  if (auto valid = ValidateDatabaseType(cfg.db_type); !valid) return valid.Error();
  return cfg;
}

auto ServerConfig::Load(int argc, char* argv[]) -> Result<ServerConfig> {
  std::filesystem::path config_path;
  for (int i = 1; i < argc - 1; ++i) {
    std::string_view key = argv[i];
    if (key == "--config") {
      config_path = argv[i + 1];
      break;
    }
  }

  ServerConfig cfg;

  if (!config_path.empty()) {
    auto file_result = FromJsonFile(config_path);
    if (!file_result) return file_result.Error();
    cfg = std::move(*file_result);
  }

  auto cli_result = FromArgs(argc, argv);
  if (!cli_result) return cli_result.Error();

  auto& cli = *cli_result;

  const auto kHasCliKey = [argc, argv](std::string_view wanted) -> bool {
    for (int i = 1; i < argc; ++i) {
      std::string_view key = argv[i];
      if (!key.starts_with("--")) continue;
      key.remove_prefix(2);
      if (key == wanted) return true;
    }
    return false;
  };

  ServerConfig defaults;

  if (kHasCliKey("type")) cfg.process_type = cli.process_type;
  if (kHasCliKey("machined")) cfg.machined_address = cli.machined_address;
  if (kHasCliKey("log-level")) cfg.log_level = cli.log_level;
  if (kHasCliKey("login-rate-limit-trusted-cidr"))
    cfg.login_rate_limit_trusted_cidrs = cli.login_rate_limit_trusted_cidrs;

  for (const auto& field : kCliFields) {
    if (kHasCliKey(field.key)) std::visit([&](auto m) { cfg.*m = cli.*m; }, field.ptr);
  }

  if (cfg.process_name.empty()) cfg.process_name = std::string(ProcessTypeName(cfg.process_type));

  if (cfg.db_type == "mysql" && cfg.db_mysql_password.empty()) {
    auto password = ReadEnvironmentVariable("ATLAS_DB_MYSQL_PASSWORD");
    if (!password) return password.Error();
    cfg.db_mysql_password = std::move(*password);
  }

  if (cfg.machined_address == Address::kNone || cfg.machined_address == defaults.machined_address) {
    auto default_addr = Address::Resolve("127.0.0.1", 20018);
    if (default_addr) cfg.machined_address = *default_addr;
  }

  return cfg;
}

auto ServerConfig::ConfigureExternalTransport(NetworkInterface& network) const -> Result<void> {
  const bool certificate = !external_dtls_certificate.empty();
  const bool key = !external_dtls_private_key.empty();
  if (certificate != key)
    return Error{ErrorCode::kInvalidArgument,
                 "External DTLS requires both certificate and private key"};
  if (!certificate) {
    if (is_production)
      return Error{ErrorCode::kPermissionDenied,
                   "Production public ingress requires DTLS credentials"};
    return {};
  }
  auto context = TlsContext::Server(external_dtls_certificate.string(),
                                    external_dtls_private_key.string(), TlsTransport::kDatagram);
  if (!context) return context.Error();
  auto configured = network.ConfigureDtls(*context);
  if (!configured) return configured.Error();
  network.SetRateLimit(1000);
  return {};
}

}  // namespace atlas
