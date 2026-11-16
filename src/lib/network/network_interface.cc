#include "network/network_interface.h"

#include <algorithm>
#include <array>

#include "foundation/log.h"
#include "foundation/profiler.h"
#include "network/event_dispatcher.h"
#include "network/protocol_version.h"
#include "network/reliable_udp.h"
#include "network/tcp_channel.h"
#include "network/udp_channel.h"

namespace atlas {

namespace {

void ApplyRudpProfile(ReliableUdpChannel& channel, const NetworkInterface::RudpProfile& profile) {
  channel.SetNocwnd(profile.nocwnd);
  channel.SetSendWindow(profile.send_window);
  channel.SetRecvWindow(profile.recv_window);
  channel.SetMtu(profile.mtu);
  channel.SetDeferredFlushThreshold(profile.deferred_flush_threshold);
}

}  // namespace

NetworkInterface::NetworkInterface(EventDispatcher& dispatcher)
    : dispatcher_(dispatcher), registration_(dispatcher_.AddFrequentTask(this)) {}

NetworkInterface::~NetworkInterface() {
  registration_.Reset();

  channels_.clear();
  channels_by_id_.clear();
  condemned_rudp_by_addr_.clear();
  condemned_.clear();
  dtls_sessions_.clear();

  if (tcp_listen_socket_) {
    (void)dispatcher_.Deregister(tcp_listen_socket_->Fd());
    tcp_listen_socket_->Close();
    tcp_listen_socket_.reset();
  }

  if (udp_socket_) {
    (void)dispatcher_.Deregister(udp_socket_->Fd());
    udp_socket_->Close();
    udp_socket_.reset();
  }

  if (rudp_socket_) {
    (void)dispatcher_.Deregister(rudp_socket_->Fd());
    rudp_socket_->Close();
    rudp_socket_.reset();
  }
}

auto NetworkInterface::StartTcpServer(const Address& addr) -> Result<void> {
  if (tcp_listen_socket_) {
    (void)dispatcher_.Deregister(tcp_listen_socket_->Fd());
    tcp_listen_socket_->Close();
    tcp_listen_socket_.reset();
  }

  auto sock = Socket::CreateTcp();
  if (!sock) {
    return sock.Error();
  }

  if (auto r = sock->SetReuseAddr(true); !r) return r.Error();
  if (auto r = sock->SetNonBlocking(true); !r) return r.Error();

  auto bind_result = sock->Bind(addr);
  if (!bind_result) {
    return bind_result.Error();
  }

  auto listen_result = sock->Listen();
  if (!listen_result) {
    return listen_result.Error();
  }

  auto local = sock->LocalAddress();
  if (!local) {
    return local.Error();
  }
  tcp_address_ = *local;

  tcp_listen_socket_ = std::move(*sock);

  auto reg = dispatcher_.RegisterReader(tcp_listen_socket_->Fd(),
                                        [this](FdHandle, IOEvent) { OnTcpAccept(); });
  if (!reg) {
    return reg.Error();
  }

  ATLAS_LOG_INFO("TCP server listening on {}", tcp_address_.ToString());
  return Result<void>{};
}

auto NetworkInterface::ConnectTcp(const Address& addr) -> Result<TcpChannel*> {
  if (shutting_down_) {
    return Error(ErrorCode::kChannelCondemned, "Shutting down");
  }

  auto sock = Socket::CreateTcp();
  if (!sock) {
    return sock.Error();
  }

  if (auto r = sock->SetNonBlocking(true); !r) return r.Error();
  if (auto r = sock->SetNoDelay(true); !r) return r.Error();

  auto conn = sock->Connect(addr);
  if (!conn && conn.Error().Code() != ErrorCode::kWouldBlock) {
    return conn.Error();
  }

  auto channel =
      make_intrusive<TcpChannel>(dispatcher_, interface_table_, std::move(*sock), addr);
  channel->SetChannelId(next_channel_id_++);

  auto reg = dispatcher_.RegisterReader(channel->Fd(),
                                        [this, ch = channel.get()](FdHandle, IOEvent events) {
                                          if ((events & IOEvent::kReadable) != IOEvent::kNone) {
                                            ATLAS_PROFILE_ZONE_N("TcpChannel::OnReadable");
                                            ch->OnReadable();
                                          }
                                          if ((events & IOEvent::kWritable) != IOEvent::kNone) {
                                            ATLAS_PROFILE_ZONE_N("TcpChannel::OnWritable");
                                            ch->OnWritable();
                                          }
                                          FlushDirtySendChannels();
                                        });
  if (!reg) {
    return reg.Error();
  }

  channel->SetDisconnectCallback([this](Channel& ch) { OnChannelDisconnect(ch); });
  channel->SetMarkDirtyCallback([this](Channel& ch) { MarkChannelDirty(ch); });
  channel->Activate();

  auto* raw = channel.get();
  channels_by_id_[raw->ChannelId()] = raw;
  channels_[addr] = std::move(channel);
  return raw;
}

auto NetworkInterface::StartUdp(const Address& addr) -> Result<void> {
  auto sock = Socket::CreateUdp();
  if (!sock) {
    return sock.Error();
  }

  if (auto r = sock->SetReuseAddr(true); !r) return r.Error();
  if (auto r = sock->SetNonBlocking(true); !r) return r.Error();

  auto bind_result = sock->Bind(addr);
  if (!bind_result) {
    return bind_result.Error();
  }

  auto local = sock->LocalAddress();
  if (!local) {
    return local.Error();
  }
  udp_address_ = *local;

  udp_socket_ = std::move(*sock);

  auto reg =
      dispatcher_.RegisterReader(udp_socket_->Fd(), [this](FdHandle, IOEvent) { OnUdpReadable(); });
  if (!reg) {
    return reg.Error();
  }

  ATLAS_LOG_INFO("UDP listening on {}", udp_address_.ToString());
  return Result<void>{};
}

auto NetworkInterface::ConnectUdp(const Address& addr) -> Result<UdpChannel*> {
  if (shutting_down_) return Error(ErrorCode::kChannelCondemned, "Shutting down");

  if (!udp_socket_) {
    if (auto r = StartUdp(Address(0, 0)); !r) return r.Error();
  }

  if (auto it = channels_.find(addr); it != channels_.end()) {
    auto* udp = dynamic_cast<UdpChannel*>(it->second.get());
    if (!udp) return Error(ErrorCode::kAlreadyExists, "Channel exists with different protocol");
    return udp;
  }

  auto channel = make_intrusive<UdpChannel>(dispatcher_, interface_table_, *udp_socket_, addr);
  channel->SetChannelId(next_channel_id_++);
  channel->SetDisconnectCallback([this](Channel& ch) { OnChannelDisconnect(ch); });
  channel->SetMarkDirtyCallback([this](Channel& ch) { MarkChannelDirty(ch); });
  channel->Activate();

  auto* raw = static_cast<UdpChannel*>(channel.get());
  channels_by_id_[raw->ChannelId()] = raw;
  channels_[addr] = std::move(channel);
  return raw;
}

auto NetworkInterface::FindChannel(const Address& addr) -> Channel* {
  auto it = channels_.find(addr);
  if (it != channels_.end()) {
    return it->second.get();
  }
  return nullptr;
}

auto NetworkInterface::FindChannel(ChannelId id) -> Channel* {
  auto it = channels_by_id_.find(id);
  if (it != channels_by_id_.end()) {
    return it->second;
  }
  return nullptr;
}

auto NetworkInterface::ChannelCount() const -> size_t {
  return channels_.size();
}

auto NetworkInterface::TcpAddress() const -> Address {
  return tcp_address_;
}

auto NetworkInterface::UdpAddress() const -> Address {
  return udp_address_;
}

auto NetworkInterface::RudpAddress() const -> Address {
  return rudp_address_;
}

auto NetworkInterface::StartRudpServer(const Address& addr) -> Result<void> {
  return StartRudpServer(addr, RudpProfile{});
}

auto NetworkInterface::StartRudpServer(const Address& addr, const RudpProfile& accept_profile)
    -> Result<void> {
  if (dtls_context_ && !dtls_context_->IsServer())
    return Error{ErrorCode::kInvalidArgument, "DTLS client cannot start a RUDP listener"};
  if (rudp_socket_) {
    return Error(ErrorCode::kAlreadyExists, "RUDP socket already open");
  }

  auto sock = Socket::CreateUdp();
  if (!sock) return sock.Error();

  if (auto r = sock->SetReuseAddr(false); !r) return r.Error();
  if (auto r = sock->SetNonBlocking(true); !r) return r.Error();
  if (auto r = sock->SetRecvBufferSize(4 * 1024 * 1024); !r)
    ATLAS_LOG_WARNING("RUDP: failed to set recv buffer size: {}", r.Error().Message());
  if (auto r = sock->SetSendBufferSize(4 * 1024 * 1024); !r)
    ATLAS_LOG_WARNING("RUDP: failed to set send buffer size: {}", r.Error().Message());

  if (auto r = sock->Bind(addr); !r) return r.Error();

  auto local = sock->LocalAddress();
  if (!local) return local.Error();
  rudp_address_ = *local;

  rudp_socket_ = std::move(*sock);
  rudp_server_mode_ = true;
  rudp_accept_profile_ = accept_profile;

  auto reg = dispatcher_.RegisterReader(rudp_socket_->Fd(),
                                        [this](FdHandle, IOEvent) { OnRudpReadable(); });
  if (!reg) return reg.Error();

  ATLAS_LOG_INFO("RUDP server listening on {}", rudp_address_.ToString());
  return Result<void>{};
}

auto NetworkInterface::ConnectRudp(const Address& addr) -> Result<ReliableUdpChannel*> {
  return ConnectRudp(addr, RudpProfile{});
}

auto NetworkInterface::ConfigureDtls(std::shared_ptr<TlsContext> context, std::string peer_identity)
    -> Result<void> {
  if (rudp_socket_ || !channels_.empty() || dtls_context_)
    return Error{ErrorCode::kAlreadyExists, "Configure DTLS before opening any RUDP socket"};
  if (!context || !context->IsDatagram() ||
      (!context->IsServer() &&
       (peer_identity.empty() || peer_identity.find('\0') != std::string::npos)))
    return Error{ErrorCode::kInvalidArgument,
                 "DTLS context and client target identity are required"};
  dtls_context_ = std::move(context);
  dtls_peer_identity_ = std::move(peer_identity);
  return {};
}

auto NetworkInterface::IsRudpReady(const Address& addr) const -> bool {
  if (!channels_.contains(addr)) return false;
  if (!dtls_context_) return true;
  const auto peer = dtls_sessions_.find(addr);
  return peer != dtls_sessions_.end() && peer->second.session->IsReady();
}

auto NetworkInterface::CreateDtlsSession(const Address& peer) -> Result<void> {
  if (dtls_sessions_.contains(peer)) return {};
  const auto pending = std::ranges::count_if(
      dtls_sessions_, [](const auto& entry) { return !entry.second.session->IsReady(); });
  if (pending >= static_cast<std::ptrdiff_t>(kMaxDtlsHandshakes) ||
      dtls_sessions_.size() >= kMaxChannels)
    return Error{ErrorCode::kRateLimited, "DTLS handshake or session capacity reached"};
  auto session = TlsStream::Create(dtls_context_, dtls_peer_identity_);
  if (!session) return session.Error();
  const auto now = Clock::now();
  dtls_sessions_.emplace(
      peer, DtlsPeer{std::move(*session), now + std::chrono::seconds(10), TimePoint::max(), {}});
  auto started = dtls_sessions_.at(peer).session->Handshake();
  if (!started) {
    dtls_sessions_.erase(peer);
    return started.Error();
  }
  auto flushed = FlushDtls(peer);
  if (!flushed) dtls_sessions_.erase(peer);
  return flushed;
}

auto NetworkInterface::AcceptDtlsDatagram(const Address& peer, std::span<const std::byte> data)
    -> Result<bool> {
  auto session = TlsStream::Create(dtls_context_);
  if (!session) return session.Error();
  if (auto fed = (*session)->FeedCiphertext(data); !fed) return fed.Error();
  auto verified = (*session)->ListenDatagram(peer);
  if (!verified) return verified.Error();
  if (!*verified) {
    std::array<std::byte, 1200> response{};
    auto output = (*session)->DrainCiphertext(response);
    if (output && *output <= data.size())
      (void)rudp_socket_->SendTo(std::span(response).first(*output), peer);
    return false;
  }
  const auto pending = std::ranges::count_if(
      dtls_sessions_, [](const auto& entry) { return !entry.second.session->IsReady(); });
  if (pending >= static_cast<std::ptrdiff_t>(kMaxDtlsHandshakes) ||
      dtls_sessions_.size() >= kMaxChannels)
    return Error{ErrorCode::kRateLimited, "DTLS handshake or session capacity reached"};
  dtls_sessions_.emplace(
      peer,
      DtlsPeer{std::move(*session), Clock::now() + std::chrono::seconds(10), TimePoint::max(), {}});
  return true;
}

auto NetworkInterface::FlushDtls(const Address& address) -> Result<void> {
  auto& peer = dtls_sessions_.at(address);
  std::array<std::byte, 1200> buffer{};
  for (int i = 0; i < 64; ++i) {
    if (peer.pending_ciphertext.empty()) {
      auto output = peer.session->DrainCiphertext(buffer);
      if (!output) {
        if (output.Error().Code() != ErrorCode::kWouldBlock) return output.Error();
        break;
      }
      peer.pending_ciphertext.assign(buffer.begin(), buffer.begin() + *output);
    }
    auto sent = rudp_socket_->SendTo(peer.pending_ciphertext, address);
    if (!sent) {
      if (sent.Error().Code() == ErrorCode::kWouldBlock) break;
      return sent.Error();
    }
    if (*sent != peer.pending_ciphertext.size())
      return Error{ErrorCode::kIoError, "DTLS UDP send was incomplete"};
    peer.pending_ciphertext.clear();
  }
  const auto timeout = peer.session->RetransmitTimeout();
  peer.next_timeout = timeout ? Clock::now() + *timeout : TimePoint::max();
  return {};
}

auto NetworkInterface::SendDtls(const Address& peer, std::span<const std::byte> data)
    -> Result<std::size_t> {
  auto found = dtls_sessions_.find(peer);
  if (found == dtls_sessions_.end())
    return Error{ErrorCode::kConnectionReset, "DTLS peer is unavailable"};
  auto sent = found->second.session->Write(data);
  if (!sent) return sent.Error();
  if (auto flushed = FlushDtls(peer); !flushed) return flushed.Error();
  return *sent;
}

void NetworkInterface::FailDtls(const Address& peer) {
  dtls_sessions_.erase(peer);
  if (auto* channel = FindChannel(peer)) OnChannelDisconnect(*channel);
}

void NetworkInterface::ProcessDtlsTimers() {
  const auto now = Clock::now();
  std::vector<Address> failed;
  for (auto& [address, peer] : dtls_sessions_) {
    const bool attached = channels_.contains(address) || condemned_rudp_by_addr_.contains(address);
    if ((!attached && peer.channel_attached) ||
        (!peer.session->IsReady() && now >= peer.deadline) || (!attached && now >= peer.deadline)) {
      failed.push_back(address);
      continue;
    }
    if (now >= peer.next_timeout && !peer.session->HandleTimeout()) {
      failed.push_back(address);
      continue;
    }
    if (!FlushDtls(address)) failed.push_back(address);
  }
  for (const auto& address : failed) FailDtls(address);
}

void NetworkInterface::CloseRudpSocket() {
  // Drop condemned channels first; their channels still reference the socket.
  condemned_rudp_by_addr_.clear();
  condemned_.clear();
  dtls_sessions_.clear();
  if (rudp_socket_) {
    (void)dispatcher_.Deregister(rudp_socket_->Fd());
    rudp_socket_->Close();
    rudp_socket_.reset();
  }
}

auto NetworkInterface::ConnectRudp(const Address& addr, const RudpProfile& profile)
    -> Result<ReliableUdpChannel*> {
  if (shutting_down_) return Error(ErrorCode::kChannelCondemned, "Shutting down");
  if (dtls_context_ && dtls_context_->IsServer())
    return Error{ErrorCode::kInvalidArgument, "DTLS listener cannot initiate an outbound peer"};

  if (!rudp_socket_) {
    auto sock = Socket::CreateUdp();
    if (!sock) return sock.Error();

    if (auto r = sock->SetReuseAddr(false); !r) return r.Error();
    if (auto r = sock->SetNonBlocking(true); !r) return r.Error();
    if (auto r = sock->SetRecvBufferSize(4 * 1024 * 1024); !r)
      ATLAS_LOG_WARNING("RUDP: failed to set recv buffer size: {}", r.Error().Message());
    if (auto r = sock->SetSendBufferSize(4 * 1024 * 1024); !r)
      ATLAS_LOG_WARNING("RUDP: failed to set send buffer size: {}", r.Error().Message());

    const Address kBindAddr = rudp_client_bind_address_.value_or(Address(0, 0));
    if (auto r = sock->Bind(kBindAddr); !r) return r.Error();

    auto local = sock->LocalAddress();
    if (!local) return local.Error();
    rudp_address_ = *local;

    rudp_socket_ = std::move(*sock);

    auto reg = dispatcher_.RegisterReader(rudp_socket_->Fd(),
                                          [this](FdHandle, IOEvent) { OnRudpReadable(); });
    if (!reg) return reg.Error();
  }

  if (auto it = channels_.find(addr); it != channels_.end()) {
    auto* rudp = dynamic_cast<ReliableUdpChannel*>(it->second.get());
    if (!rudp) return Error(ErrorCode::kAlreadyExists, "Channel exists with different protocol");
    return rudp;
  }

  if (dtls_context_) {
    if (dtls_sessions_.contains(addr))
      return Error{ErrorCode::kWouldBlock, "DTLS peer is still draining"};
    if (auto created = CreateDtlsSession(addr); !created) return created.Error();
  }
  auto channel =
      make_intrusive<ReliableUdpChannel>(dispatcher_, interface_table_, *rudp_socket_, addr);
  channel->SetChannelId(next_channel_id_++);
  channel->SetDisconnectCallback([this](Channel& ch) { OnChannelDisconnect(ch); });
  channel->SetMarkDirtyCallback([this](Channel& ch) { MarkChannelDirty(ch); });
  ApplyRudpProfile(*channel, profile);
  if (dtls_context_)
    channel->SetDatagramSender(
        [this, addr](std::span<const std::byte> data) { return SendDtls(addr, data); });
  channel->Activate();

  auto* raw = static_cast<ReliableUdpChannel*>(channel.get());
  channels_by_id_[raw->ChannelId()] = raw;
  channels_[addr] = std::move(channel);
  if (dtls_context_) dtls_sessions_.at(addr).channel_attached = true;
  return raw;
}

auto NetworkInterface::InternetRudpProfile() -> RudpProfile {
  RudpProfile profile;
  profile.mtu = 470;
  profile.deferred_flush_threshold = 32 * 1024;
  return profile;
}

auto NetworkInterface::ClusterRudpProfile() -> RudpProfile {
  RudpProfile profile;
  profile.nocwnd = true;
  profile.send_window = 4096;
  profile.recv_window = 4096;
  profile.mtu = 1400;
  profile.deferred_flush_threshold = 60 * 1024;
  return profile;
}

void NetworkInterface::SetRudpClientBindAddress(const Address& addr) {
  rudp_client_bind_address_ = addr;
}

auto NetworkInterface::ConnectRudpNocwnd(const Address& addr) -> Result<ReliableUdpChannel*> {
  if (auto it = channels_.find(addr); it != channels_.end()) {
    auto* rudp = dynamic_cast<ReliableUdpChannel*>(it->second.get());
    if (!rudp) return Error(ErrorCode::kAlreadyExists, "Channel exists with different protocol");
    return rudp;
  }

  auto result = ConnectRudp(addr, ClusterRudpProfile());
  if (!result) return result;
  return result;
}

void NetworkInterface::SetRateLimit(uint32_t max_per_second) {
  rate_limit_ = max_per_second;
}

void NetworkInterface::SetAcceptCallback(AcceptCallback cb) {
  accept_callback_ = std::move(cb);
}

void NetworkInterface::SetDisconnectCallback(DisconnectCallback cb) {
  disconnect_callback_ = std::move(cb);
}

void NetworkInterface::PrepareForShutdown() {
  StopAccepting();
  shutting_down_ = true;

  while (!channels_.empty()) {
    CondemnChannel(channels_.begin()->first);
  }

  ATLAS_LOG_INFO("NetworkInterface preparing for shutdown, {} channels condemned",
                 condemned_.size());
}

auto NetworkInterface::PendingSendCount() const -> std::size_t {
  auto pending = [this](Channel* channel) {
    if (channel->HasUnackedReliablePackets() || dirty_channels_.contains(channel)) return true;
    const auto* tcp = dynamic_cast<const TcpChannel*>(channel);
    return tcp != nullptr && tcp->WriteBufferSize() != 0;
  };
  std::size_t count = 0;
  for (const auto& [_, channel] : channels_) {
    if (pending(channel.get())) ++count;
  }
  for (const auto& condemned : condemned_) {
    if (pending(condemned.channel.get())) ++count;
  }
  for (const auto& [_, peer] : dtls_sessions_) {
    if (!peer.session->IsReady() || !peer.pending_ciphertext.empty()) ++count;
  }
  return count;
}

void NetworkInterface::DoTask() {
  ATLAS_PROFILE_ZONE_N("NetworkInterface::DoTask");
  ProcessCondemnedChannels();
  ProcessDtlsTimers();

  // Backup drain so yielded RUDP channels don't wait for the next datagram.
  if (!hot_channels_.empty()) {
    DrainHotChannels(Clock::now() + kReadableCallbackBudget / 2);
  }

  if (rate_limit_ > 0) {
    auto now = Clock::now();
    if (now - last_rate_cleanup_ >= kRateCleanupInterval) {
      CleanupStaleRateTrackers();
      last_rate_cleanup_ = now;
    }
  }
}

void NetworkInterface::OnTcpAccept() {
  ATLAS_PROFILE_ZONE_N("NetworkInterface::OnTcpAccept");
  std::size_t accepts = 0;
  while (true) {
    if (CallbackBudgetExhausted(accepts, kMaxAcceptsPerCallback)) {
      break;
    }

    auto result = tcp_listen_socket_->Accept();
    if (!result) {
      if (result.Error().Code() == ErrorCode::kWouldBlock) {
        break;
      }
      ATLAS_LOG_WARNING("TCP accept error: {}", result.Error().Message());
      break;
    }

    auto& [peer_sock, peer_addr] = *result;
    ++accepts;

    if (rate_limit_ > 0 && !CheckRateLimit(peer_addr.Ip())) {
      ATLAS_LOG_WARNING("Rate limited connection from {}", peer_addr.ToString());
      continue;
    }

    if (shutting_down_ || !accepting_connections_) {
      continue;
    }

    if (auto nd = peer_sock.SetNoDelay(true); !nd) {
      ATLAS_LOG_WARNING("Failed to set TCP_NODELAY on accepted connection from {}: {}",
                        peer_addr.ToString(), nd.Error().Message());
    }

    auto channel = make_intrusive<TcpChannel>(dispatcher_, interface_table_, std::move(peer_sock),
                                              peer_addr);
    channel->SetChannelId(next_channel_id_++);

    auto reg = dispatcher_.RegisterReader(channel->Fd(),
                                          [this, ch = channel.get()](FdHandle, IOEvent events) {
                                            if ((events & IOEvent::kReadable) != IOEvent::kNone) {
                                              ATLAS_PROFILE_ZONE_N("TcpChannel::OnReadable");
                                              ch->OnReadable();
                                            }
                                            if ((events & IOEvent::kWritable) != IOEvent::kNone) {
                                              ATLAS_PROFILE_ZONE_N("TcpChannel::OnWritable");
                                              ch->OnWritable();
                                            }
                                            FlushDirtySendChannels();
                                          });
    if (!reg) {
      ATLAS_LOG_ERROR("Failed to register channel fd: {}", reg.Error().Message());
      continue;
    }

    channel->SetDisconnectCallback([this](Channel& ch) { OnChannelDisconnect(ch); });
    channel->SetMarkDirtyCallback([this](Channel& ch) { MarkChannelDirty(ch); });
    channel->Activate();

    ATLAS_LOG_DEBUG("Accepted TCP connection from {}", peer_addr.ToString());
    channels_by_id_[channel->ChannelId()] = channel.get();
    channels_[peer_addr] = std::move(channel);
    if (accept_callback_) {
      accept_callback_(*channels_[peer_addr]);
    }
  }
}

void NetworkInterface::OnUdpReadable() {
  ATLAS_PROFILE_ZONE_N("NetworkInterface::OnUdpReadable");
  const auto deadline = Clock::now() + kReadableCallbackBudget;
  auto recv_buffer = DatagramRecvBuffer();
  std::size_t datagrams = 0;
  while (datagrams < kMaxDatagramsPerCallback) {
    if (Clock::now() >= deadline) break;

    auto result = udp_socket_->RecvFrom(recv_buffer);
    if (!result) {
      if (result.Error().Code() == ErrorCode::kWouldBlock) {
        break;
      }
      if (result.Error().Code() == ErrorCode::kConnectionReset) {
        ++datagrams;
        continue;
      }
      ATLAS_LOG_WARNING("UDP recv error: {}", result.Error().Message());
      break;
    }

    auto [bytes, src_addr] = *result;
    ++datagrams;
    if (!HasConnectionProtocolHeader(recv_buffer.first(bytes))) {
      continue;
    }

    if (rate_limit_ > 0 && !CheckRateLimit(src_addr.Ip())) {
      continue;
    }

    auto it = channels_.find(src_addr);
    if (it == channels_.end()) {
      if (shutting_down_ || !accepting_connections_ || channels_.size() >= kMaxChannels) {
        continue;
      }

      auto channel =
          make_intrusive<UdpChannel>(dispatcher_, interface_table_, *udp_socket_, src_addr);
      channel->SetChannelId(next_channel_id_++);
      channel->SetDisconnectCallback([this](Channel& ch) { OnChannelDisconnect(ch); });
      channel->SetMarkDirtyCallback([this](Channel& ch) { MarkChannelDirty(ch); });
      channel->Activate();
      channels_by_id_[channel->ChannelId()] = channel.get();
      auto [inserted_it, _] = channels_.emplace(src_addr, std::move(channel));
      it = inserted_it;
    }

    auto* udp_ch = static_cast<UdpChannel*>(it->second.get());
    udp_ch->OnDatagramReceived(recv_buffer.first(bytes));
  }

  FlushDirtySendChannels();
}

void NetworkInterface::OnRudpReadable() {
  ATLAS_PROFILE_ZONE_N("NetworkInterface::OnRudpReadable");
  const auto deadline = Clock::now() + kReadableCallbackBudget;
  auto recv_buffer = DatagramRecvBuffer();
  std::size_t datagrams = 0;
  while (datagrams < kMaxDatagramsPerCallback) {
    if (!rudp_socket_) break;
    if (Clock::now() >= deadline) break;

    auto result = rudp_socket_->RecvFrom(recv_buffer);
    if (!result) {
      if (result.Error().Code() == ErrorCode::kWouldBlock) break;
      if (result.Error().Code() == ErrorCode::kConnectionReset) {
        ++datagrams;
        continue;
      }
      ATLAS_LOG_WARNING("RUDP recv error: {}", result.Error().Message());
      break;
    }

    auto [bytes, src_addr] = *result;
    ++datagrams;
    if (!dtls_context_) {
      DispatchRudpDatagram(src_addr, recv_buffer.first(bytes), deadline);
      continue;
    }
    if (rate_limit_ > 0 && !CheckRateLimit(src_addr.Ip())) continue;
    if (!dtls_sessions_.contains(src_addr)) {
      if (!rudp_server_mode_ || !accepting_connections_ || shutting_down_) continue;
      auto accepted = AcceptDtlsDatagram(src_addr, recv_buffer.first(bytes));
      if (!accepted || !*accepted) continue;
    } else {
      if (!dtls_sessions_.at(src_addr).session->FeedCiphertext(recv_buffer.first(bytes))) continue;
    }
    auto& peer = dtls_sessions_.at(src_addr);
    auto handshake = peer.session->Handshake();
    if (!handshake || !FlushDtls(src_addr)) {
      FailDtls(src_addr);
      continue;
    }
    if (!*handshake) continue;
    auto* session = peer.session.get();
    std::array<std::byte, 1200> plaintext{};
    for (int i = 0; i < 64 && Clock::now() < deadline; ++i) {
      auto decrypted = session->Read(plaintext);
      if (!decrypted) {
        if (decrypted.Error().Code() != ErrorCode::kWouldBlock) FailDtls(src_addr);
        break;
      }
      DispatchRudpDatagram(src_addr, std::span(plaintext).first(*decrypted), deadline);
      const auto current = dtls_sessions_.find(src_addr);
      if (current == dtls_sessions_.end() || current->second.session.get() != session) break;
    }
  }

  if (!hot_channels_.empty()) {
    DrainHotChannels(deadline);
  }

  FlushDirtySendChannels();
}

void NetworkInterface::DispatchRudpDatagram(const Address& source, std::span<const std::byte> data,
                                            TimePoint deadline) {
  if (!HasConnectionProtocolHeader(data)) return;

  if (!dtls_context_ && rate_limit_ > 0 && !CheckRateLimit(source.Ip())) return;

  auto it = channels_.find(source);
  if (it == channels_.end()) {
    if (auto cond_it = condemned_rudp_by_addr_.find(source);
        cond_it != condemned_rudp_by_addr_.end()) {
      cond_it->second->OnDatagramReceived(data, deadline);
      return;
    }

    if (!rudp_server_mode_ || shutting_down_ || !accepting_connections_ ||
        channels_.size() >= kMaxChannels)
      return;
    if (dtls_context_ && dtls_sessions_.at(source).channel_attached) return;

    auto channel =
        make_intrusive<ReliableUdpChannel>(dispatcher_, interface_table_, *rudp_socket_, source);
    channel->SetChannelId(next_channel_id_++);
    channel->SetDisconnectCallback([this](Channel& ch) { OnChannelDisconnect(ch); });
    channel->SetMarkDirtyCallback([this](Channel& ch) { MarkChannelDirty(ch); });
    channel->SetHotCallback([this](ReliableUdpChannel& ch) { hot_channels_.insert(&ch); });
    ApplyRudpProfile(*channel, rudp_accept_profile_);
    if (dtls_context_)
      channel->SetDatagramSender(
          [this, source](std::span<const std::byte> bytes) { return SendDtls(source, bytes); });
    channel->Activate();

    ATLAS_LOG_DEBUG("RUDP: new peer {}", source.ToString());
    channels_by_id_[channel->ChannelId()] = channel.get();
    auto [inserted_it, _] = channels_.emplace(source, std::move(channel));
    it = inserted_it;
    if (dtls_context_) dtls_sessions_.at(source).channel_attached = true;
    if (accept_callback_) accept_callback_(*it->second);
  }

  auto channel = it->second;
  auto* rudp_ch = static_cast<ReliableUdpChannel*>(channel.get());
  rudp_ch->OnDatagramReceived(data, deadline);
}

void NetworkInterface::FlushDirtySendChannels() {
  // Move-and-clear so a channel that re-dirties itself mid-flush lands
  // in a fresh set instead of the iterator we're still walking.
  if (dirty_channels_.empty()) return;
  ATLAS_PROFILE_ZONE_N("NetworkInterface::FlushDirtySendChannels");
  std::unordered_set<Channel*> snapshot;
  snapshot.swap(dirty_channels_);
  for (Channel* ch : snapshot) {
    if (auto r = ch->FlushDeferred(); !r) {
      ATLAS_LOG_DEBUG("FlushDirty: channel {} returned {}", ch->RemoteAddress().ToString(),
                      r.Error().Message());
    }
  }
}

void NetworkInterface::DrainHotChannels(TimePoint deadline) {
  for (auto it = hot_channels_.begin(); it != hot_channels_.end();) {
    if (Clock::now() >= deadline) break;
    ReliableUdpChannel* ch = *it;
    const bool still_hot = ch->FlushReceiveBuffer(deadline);
    if (!still_hot) {
      it = hot_channels_.erase(it);
    } else {
      ++it;
    }
  }
}

void NetworkInterface::OnChannelDisconnect(Channel& channel) {
  auto addr = channel.RemoteAddress();
  CondemnChannel(addr);

  if (disconnect_callback_) {
    disconnect_callback_(channel);
  }
}

void NetworkInterface::CondemnChannel(Address addr) {
  auto it = channels_.find(addr);
  if (it == channels_.end()) {
    return;
  }

  auto channel = std::move(it->second);
  channels_.erase(it);
  channels_by_id_.erase(channel->ChannelId());

  auto* rudp = dynamic_cast<ReliableUdpChannel*>(channel.get());
  if (rudp) {
    hot_channels_.erase(rudp);
  }
  dirty_channels_.erase(channel.get());

  const bool kSharedFd = (rudp_socket_ && channel->Fd() == rudp_socket_->Fd()) ||
                         (udp_socket_ && channel->Fd() == udp_socket_->Fd());
  if (!kSharedFd) (void)dispatcher_.Deregister(channel->Fd());
  channel->Condemn();

  if (rudp && rudp->HasUnackedReliablePackets()) {
    condemned_rudp_by_addr_[addr] = rudp;
  }
  condemned_.push_back({std::move(channel), Clock::now()});

  while (condemned_.size() > kMaxCondemnedChannels) {
    ATLAS_LOG_WARNING(
        "NetworkInterface: condemned channel list at capacity ({}), "
        "force-closing oldest entry",
        kMaxCondemnedChannels);
    EraseCondemnedRudpIndex(condemned_.front().channel.get());
    condemned_.pop_front();
  }
}

void NetworkInterface::EraseCondemnedRudpIndex(Channel* channel) {
  auto* rudp = dynamic_cast<ReliableUdpChannel*>(channel);
  if (!rudp) return;
  auto idx_it = condemned_rudp_by_addr_.find(rudp->RemoteAddress());
  if (idx_it != condemned_rudp_by_addr_.end() && idx_it->second == rudp) {
    condemned_rudp_by_addr_.erase(idx_it);
  }
}

auto NetworkInterface::ShouldDeleteCondemned(const Channel& channel, TimePoint condemned_at,
                                             TimePoint now) const -> bool {
  return !channel.HasUnackedReliablePackets() || channel.HasRemoteFailed() ||
         (now - condemned_at) >= kCondemnAgeLimit;
}

void NetworkInterface::ProcessCondemnedChannels() {
  const auto now = Clock::now();
  for (auto it = condemned_.begin(); it != condemned_.end();) {
    if (!ShouldDeleteCondemned(*it->channel, it->condemned_at, now)) {
      ++it;
      continue;
    }
    if ((now - it->condemned_at) >= kCondemnAgeLimit && it->channel->HasUnackedReliablePackets()) {
      ATLAS_LOG_WARNING("Condemned channel {} hit age limit with unacked packets — discarding",
                        it->channel->RemoteAddress().ToString());
    }
    EraseCondemnedRudpIndex(it->channel.get());
    it = condemned_.erase(it);
  }
}

auto NetworkInterface::DatagramRecvBuffer() -> std::span<std::byte> {
  if (!datagram_recv_scratch_) {
    datagram_recv_scratch_ = StreamBufferPool::Instance().Acquire(kMaxDatagramSize);
  }

  return {datagram_recv_scratch_.data(), datagram_recv_scratch_.Capacity()};
}

auto NetworkInterface::CheckRateLimit(uint32_t ip) -> bool {
  static constexpr std::size_t kMaxRateTrackers = 100'000;

  if (rate_trackers_.size() >= kMaxRateTrackers && !rate_trackers_.contains(ip)) {
    return false;
  }

  auto now = Clock::now();
  auto& tracker = rate_trackers_[ip];

  if (now - tracker.window_start >= std::chrono::seconds(1)) {
    tracker.count = 0;
    tracker.window_start = now;
  }

  ++tracker.count;
  return tracker.count <= rate_limit_;
}

auto NetworkInterface::CallbackBudgetExhausted(std::size_t processed, std::size_t budget) -> bool {
  return processed >= budget;
}

void NetworkInterface::CleanupStaleRateTrackers() {
  auto now = Clock::now();
  std::erase_if(rate_trackers_, [now](const auto& kv) {
    return (now - kv.second.window_start) > std::chrono::seconds(2);
  });
}

}  // namespace atlas
