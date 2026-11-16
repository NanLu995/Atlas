#ifndef ATLAS_LIB_NET_CLIENT_CLIENT_SESSION_H_
#define ATLAS_LIB_NET_CLIENT_CLIENT_SESSION_H_

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "foundation/timer_queue.h"
#include "net_client/client_api.h"
#include "network/event_dispatcher.h"
#include "network/network_interface.h"
#include "server/entity_types.h"
#include "server/recovery_credential.h"

namespace atlas::login {
struct LoginResult;
}
namespace atlas::baseapp {
struct AuthenticateResult;
struct ClientHandoff;
struct ClientLogoutResult;
}

namespace atlas::net_client {

// Async Login/Auth state machine for the Unity DLL. Single-thread-owned.
// Replaces the blocking poll loop in src/client/client_app.cc.
class ClientSession {
  friend struct ClientSessionTestAccess;

 public:
  ClientSession();
  ~ClientSession();

  ClientSession(const ClientSession&) = delete;
  ClientSession& operator=(const ClientSession&) = delete;

  auto Poll() -> int32_t;
  void PrepareForDestroy();

  [[nodiscard]] auto GetState() const -> AtlasNetState { return state_; }

  // Legal only in DISCONNECTED.
  auto StartLogin(std::string_view loginapp_host, uint16_t loginapp_port, std::string_view username,
                  std::string_view password_hash, AtlasLoginResultFn callback, void* user_data)
      -> int32_t;
  auto StartResume(std::string_view loginapp_host, uint16_t loginapp_port,
                   const AtlasRecoveryCredential& credential, AtlasLoginResultFn callback,
                   void* user_data) -> int32_t;
  auto StartLogout(AtlasLogoutResultFn callback, void* user_data) -> int32_t;

  // Legal only in LOGIN_SUCCEEDED. Reads cached SessionKey + BaseApp addr.
  auto StartAuthenticate(AtlasAuthResultFn callback, void* user_data) -> int32_t;

  // Idempotent. LOGOUT fires on_disconnect with callback reason=3; USER is silent.
  auto Disconnect(AtlasDisconnectReason reason) -> int32_t;

  auto SetTransportImpairment(uint32_t one_way_latency_ms, uint32_t loss_permyriad,
                              uint32_t seed) -> int32_t;
  auto ConfigureDtls(const char* trust_file, const char* peer_identity) -> int32_t;
  auto GetRecoveryCredential(AtlasRecoveryCredential* credential) const -> int32_t;

  auto SendBaseRpc(uint32_t entity_id, uint32_t rpc_id, const uint8_t* payload, int32_t len)
      -> int32_t;

  auto SendCellRpc(uint32_t entity_id, uint32_t rpc_id, const uint8_t* payload, int32_t len)
      -> int32_t;

  auto SendMovementInput(uint32_t target_entity_id, const AtlasMovementInputFrame* frames,
                         int32_t frame_count) -> int32_t;
  auto SendMovementCorrectionReport(uint32_t target_entity_id, uint32_t acked_input_seq,
                                    uint32_t server_tick, float distance_m,
                                    uint16_t correction_flags) -> int32_t;

  // Stamped into LoginRequest so BaseApp can reject mismatched .def builds.
  void SetEntityDefDigest(const uint8_t* data, int32_t len);

  // NULL fields in `cb` are substituted with internal noops.
  auto SetCallbacks(const AtlasNetCallbacks& cb) -> int32_t;

  [[nodiscard]] auto LastError() const -> const std::string& { return last_error_; }

  auto FillStats(AtlasNetStats* out) const -> int32_t;

 private:
  auto StartLoginTransport(std::string_view host, uint16_t port, AtlasLoginResultFn callback,
                           void* user_data, std::function<Result<void>(Channel*)> send) -> int32_t;
  void OnLoginResult(const ::atlas::login::LoginResult& msg);
  void FireLoginCallback(AtlasLoginStatus status, std::string_view baseapp_host,
                         uint16_t baseapp_port, std::string_view error);
  void CancelLoginTimeout();
  void OnLogoutResult(Channel* channel, const baseapp::ClientLogoutResult& result);
  void CompleteLogout(bool success, std::string error);

  void OnAuthResult(const ::atlas::baseapp::AuthenticateResult& msg);
  void OnClientHandoff(const ::atlas::baseapp::ClientHandoff& msg);
  void FailClientHandoff(std::string error);
  void CloseHandoffSourceChannel();
  void FireAuthCallback(bool success, EntityID entity_id, uint16_t type_id, std::string_view error);
  void CancelAuthTimeout();

  void InstallDefaultHandler();
  void CloseLoginAppChannel();
  void CloseBaseAppChannel();
  void ClearSessionKey();
  void ApplyTransportImpairment(Channel* channel) const;
  [[nodiscard]] auto SendAfterHandshake(Channel* channel, std::function<Result<void>()> send)
      -> Result<void>;
  void ClearTransportSend(Channel* channel);

  EventDispatcher dispatcher_{"net_client"};
  NetworkInterface network_{dispatcher_};

  AtlasNetState state_{ATLAS_NET_STATE_DISCONNECTED};
  bool polling_{false};
  bool connection_lost_{false};
  std::optional<AtlasDisconnectReason> pending_disconnect_;

  Channel* loginapp_channel_{nullptr};
  Channel* baseapp_channel_{nullptr};
  Channel* handoff_source_channel_{nullptr};
  SessionKey handoff_credential_;
  EntityID handoff_entity_id_{kInvalidEntityID};
  uint64_t handoff_instance_{0};

  std::array<uint8_t, sizeof(SessionKey::bytes)> session_key_{};
  Address baseapp_addr_{};
  EntityID player_entity_id_{kInvalidEntityID};
  uint16_t player_type_id_{0};
  RecoveryCredential recovery_credential_;

  AtlasLoginResultFn login_callback_{nullptr};
  void* login_user_data_{nullptr};
  TimerHandle login_timeout_{};
  uint64_t next_logout_id_{1};
  uint64_t logout_request_id_{0};
  TimerHandle logout_timeout_{};
  AtlasLogoutResultFn logout_callback_{nullptr};
  void* logout_user_data_{nullptr};

  AtlasAuthResultFn auth_callback_{nullptr};
  void* auth_user_data_{nullptr};
  TimerHandle auth_timeout_{};

  AtlasNetCallbacks callbacks_{};

  std::array<uint8_t, 32> entity_def_digest_{};
  uint32_t transport_impairment_latency_ms_{0};
  uint32_t transport_impairment_loss_permyriad_{0};
  uint32_t transport_impairment_seed_{1};

  std::string last_error_;
  std::function<Result<void>()> pending_transport_send_;
  std::function<void()> pending_login_start_;
  ChannelId pending_transport_channel_{0};
  bool dtls_required_{false};
  bool dtls_configured_{false};

  static constexpr Duration kLoginTimeout{std::chrono::seconds(10)};
  static constexpr Duration kAuthTimeout{std::chrono::seconds(5)};
};

}  // namespace atlas::net_client

struct AtlasNetContext : ::atlas::net_client::ClientSession {
  uint32_t active_calls{0};
  bool destroy_requested{false};
};

#endif  // ATLAS_LIB_NET_CLIENT_CLIENT_SESSION_H_
