#include "network/tls_stream.h"

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

#include "platform/secure_random.h"

namespace atlas {
namespace {

auto TlsError() -> Error {
  ERR_clear_error();
  return Error{ErrorCode::kPermissionDenied, "TLS operation or certificate validation failed"};
}

auto ValidPath(const std::string& path) -> bool {
  return !path.empty() && path.find('\0') == std::string::npos;
}

using CookieKey = std::array<unsigned char, 32>;

auto CookieMac(SSL* ssl, const unsigned char* timestamp, unsigned char* output) -> bool {
  const auto* key = static_cast<const CookieKey*>(SSL_CTX_get_app_data(SSL_get_SSL_CTX(ssl)));
  const auto* peer = static_cast<const Address*>(SSL_get_app_data(ssl));
  if (!key || !peer) return false;
  std::array<unsigned char, 14> input{};
  std::memcpy(input.data(), timestamp, 8);
  auto ip = peer->Ip();
  auto port = peer->Port();
  std::memcpy(input.data() + 8, &ip, 4);
  input[12] = static_cast<unsigned char>(port >> 8);
  input[13] = static_cast<unsigned char>(port);
  std::size_t length = 0;
  return EVP_Q_mac(nullptr, "HMAC", nullptr, "SHA256", nullptr, key->data(), key->size(),
                   input.data(), input.size(), output, 32, &length) != nullptr &&
         length == 32;
}

auto CookieTime() -> uint64_t {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::seconds>(Clock::now().time_since_epoch()).count());
}

auto GenerateCookie(SSL* ssl, unsigned char* cookie, unsigned int* length) -> int {
  const auto now = CookieTime();
  for (int i = 0; i < 8; ++i) cookie[i] = static_cast<unsigned char>(now >> (i * 8));
  if (!CookieMac(ssl, cookie, cookie + 8)) return 0;
  *length = 40;
  return 1;
}

auto VerifyCookie(SSL* ssl, const unsigned char* cookie, unsigned int length) -> int {
  if (length != 40) return 0;
  uint64_t issued = 0;
  for (int i = 0; i < 8; ++i) issued |= static_cast<uint64_t>(cookie[i]) << (i * 8);
  const auto now = CookieTime();
  if (issued > now || now - issued > 60) return 0;
  CookieKey expected{};
  if (!CookieMac(ssl, cookie, expected.data())) return 0;
  return CRYPTO_memcmp(expected.data(), cookie + 8, expected.size()) == 0 ? 1 : 0;
}

}  // namespace

struct TlsContext::Impl {
  std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)> context{nullptr, SSL_CTX_free};
  bool server{false};
  bool datagram{false};
  CookieKey cookie_key{};
  ~Impl() { OPENSSL_cleanse(cookie_key.data(), cookie_key.size()); }
};

TlsContext::TlsContext(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
TlsContext::~TlsContext() = default;
auto TlsContext::IsDatagram() const -> bool {
  return impl_->datagram;
}
auto TlsContext::IsServer() const -> bool {
  return impl_->server;
}

auto TlsContext::Server(const std::string& certificate, const std::string& key,
                        TlsTransport transport) -> Result<std::shared_ptr<TlsContext>> {
  if (!ValidPath(certificate) || !ValidPath(key))
    return Error{ErrorCode::kInvalidArgument, "TLS certificate and key paths are required"};
  auto impl = std::make_unique<Impl>();
  impl->server = true;
  impl->datagram = transport == TlsTransport::kDatagram;
  ERR_clear_error();
  impl->context.reset(SSL_CTX_new(impl->datagram ? DTLS_server_method() : TLS_server_method()));
  auto* context = impl->context.get();
  if (context) {
    SSL_CTX_set_default_passwd_cb(context, [](char*, int, int, void*) { return 0; });
  }
  const int version = impl->datagram ? DTLS1_2_VERSION : TLS1_3_VERSION;
  if (!context || SSL_CTX_set_min_proto_version(context, version) != 1 ||
      SSL_CTX_set_max_proto_version(context, version) != 1 ||
      SSL_CTX_use_certificate_chain_file(context, certificate.c_str()) != 1 ||
      SSL_CTX_use_PrivateKey_file(context, key.c_str(), SSL_FILETYPE_PEM) != 1 ||
      SSL_CTX_check_private_key(context) != 1)
    return TlsError();
  SSL_CTX_set_options(context, SSL_OP_NO_COMPRESSION | SSL_OP_NO_RENEGOTIATION);
  SSL_CTX_set_session_cache_mode(context, SSL_SESS_CACHE_OFF);
  SSL_CTX_set_num_tickets(context, 0);
  if (impl->datagram) {
    auto random = FillSecureRandom(std::as_writable_bytes(std::span(impl->cookie_key)));
    if (!random) return random.Error();
    SSL_CTX_set_app_data(context, &impl->cookie_key);
    SSL_CTX_set_cookie_generate_cb(context, GenerateCookie);
    SSL_CTX_set_cookie_verify_cb(context, VerifyCookie);
  }
  return std::shared_ptr<TlsContext>(new TlsContext(std::move(impl)));
}

auto TlsContext::Client(const std::string& trust_file, TlsTransport transport)
    -> Result<std::shared_ptr<TlsContext>> {
  if (!ValidPath(trust_file))
    return Error{ErrorCode::kInvalidArgument, "TLS trust file is required"};
  auto impl = std::make_unique<Impl>();
  impl->datagram = transport == TlsTransport::kDatagram;
  ERR_clear_error();
  impl->context.reset(SSL_CTX_new(impl->datagram ? DTLS_client_method() : TLS_client_method()));
  auto* context = impl->context.get();
  const int version = impl->datagram ? DTLS1_2_VERSION : TLS1_3_VERSION;
  if (!context || SSL_CTX_set_min_proto_version(context, version) != 1 ||
      SSL_CTX_set_max_proto_version(context, version) != 1 ||
      SSL_CTX_load_verify_locations(context, trust_file.c_str(), nullptr) != 1)
    return TlsError();
  SSL_CTX_set_verify(context, SSL_VERIFY_PEER, nullptr);
  SSL_CTX_set_options(context, SSL_OP_NO_COMPRESSION | SSL_OP_NO_RENEGOTIATION);
  SSL_CTX_set_session_cache_mode(context, SSL_SESS_CACHE_OFF);
  return std::shared_ptr<TlsContext>(new TlsContext(std::move(impl)));
}

struct TlsStream::Impl {
  std::shared_ptr<TlsContext> context;
  std::unique_ptr<SSL, decltype(&SSL_free)> ssl{nullptr, SSL_free};
  std::unique_ptr<BIO, decltype(&BIO_free)> transport{nullptr, BIO_free};
  bool failed{false};
  bool datagram{false};
  Address cookie_peer;

  auto Check(int result) -> Error {
    const auto error = SSL_get_error(ssl.get(), result);
    if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE)
      return Error{ErrorCode::kWouldBlock, "TLS requires more transport I/O"};
    failed = true;
    if (error == SSL_ERROR_ZERO_RETURN)
      return Error{ErrorCode::kConnectionReset, "TLS peer closed"};
    return TlsError();
  }
};

TlsStream::TlsStream(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
TlsStream::~TlsStream() = default;

auto TlsStream::Create(const std::shared_ptr<TlsContext>& context, const std::string& peer_identity)
    -> Result<std::unique_ptr<TlsStream>> {
  if (!context || (!context->impl_->server && !ValidPath(peer_identity)))
    return Error{ErrorCode::kInvalidArgument, "TLS client requires a target identity"};
  auto impl = std::make_unique<Impl>();
  impl->context = context;
  impl->datagram = context->impl_->datagram;
  ERR_clear_error();
  impl->ssl.reset(SSL_new(context->impl_->context.get()));
  if (!impl->ssl) return TlsError();
  if (!context->impl_->server) {
    SSL_set_hostflags(impl->ssl.get(), X509_CHECK_FLAG_NEVER_CHECK_SUBJECT);
    if (SSL_set1_host(impl->ssl.get(), peer_identity.c_str()) != 1 ||
        SSL_set_tlsext_host_name(impl->ssl.get(), peer_identity.c_str()) != 1)
      return TlsError();
    SSL_set_connect_state(impl->ssl.get());
  } else {
    SSL_set_accept_state(impl->ssl.get());
  }
  BIO* internal = nullptr;
  BIO* external = nullptr;
  const auto paired =
      impl->datagram
          ? BIO_new_bio_dgram_pair(&internal, kCipherBufferSize, &external, kCipherBufferSize)
          : BIO_new_bio_pair(&internal, kCipherBufferSize, &external, kCipherBufferSize);
  if (paired != 1) return TlsError();
  if (impl->datagram) {
    BIO_dgram_set_no_trunc(internal, 1);
    BIO_dgram_set_no_trunc(external, 1);
    BIO_dgram_set_mtu(internal, 1200);
    SSL_set_options(impl->ssl.get(), SSL_OP_NO_QUERY_MTU);
    SSL_set_mtu(impl->ssl.get(), 1200);
  }
  impl->transport.reset(external);
  SSL_set_bio(impl->ssl.get(), internal, internal);
  SSL_set_mode(impl->ssl.get(), SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
  return std::unique_ptr<TlsStream>(new TlsStream(std::move(impl)));
}

auto TlsStream::IsReady() const -> bool {
  return !impl_->failed && SSL_is_init_finished(impl_->ssl.get()) == 1;
}

auto TlsStream::Handshake() -> Result<bool> {
  if (impl_->failed) return TlsError();
  ERR_clear_error();
  const auto result = SSL_do_handshake(impl_->ssl.get());
  if (result == 1) return true;
  auto error = impl_->Check(result);
  if (error.Code() == ErrorCode::kWouldBlock) return false;
  return error;
}

auto TlsStream::ListenDatagram(const Address& peer) -> Result<bool> {
  if (impl_->failed) return TlsError();
  if (!impl_->datagram || !impl_->context->IsServer())
    return Error{ErrorCode::kInvalidArgument, "DTLS listen requires a datagram server"};
  impl_->cookie_peer = peer;
  SSL_set_app_data(impl_->ssl.get(), &impl_->cookie_peer);
  std::unique_ptr<BIO_ADDR, decltype(&BIO_ADDR_free)> address(BIO_ADDR_new(), BIO_ADDR_free);
  if (!address) return Error{ErrorCode::kOutOfMemory, "DTLS peer address allocation failed"};
  const auto result = DTLSv1_listen(impl_->ssl.get(), address.get());
  if (result < 0) {
    impl_->failed = true;
    return TlsError();
  }
  return result > 0;
}

auto TlsStream::FeedCiphertext(std::span<const std::byte> bytes) -> Result<std::size_t> {
  if (impl_->failed) return TlsError();
  if (bytes.empty()) return std::size_t{0};
  if (impl_->datagram && bytes.size() > 1200)
    return Error{ErrorCode::kMessageTooLarge, "DTLS datagram exceeds transport MTU"};
  const auto size =
      (std::min)(bytes.size(), static_cast<std::size_t>((std::numeric_limits<int>::max)()));
  const auto result = BIO_write(impl_->transport.get(), bytes.data(), static_cast<int>(size));
  if (result > 0) return static_cast<std::size_t>(result);
  return Error{ErrorCode::kWouldBlock, "TLS ciphertext input buffer full"};
}

auto TlsStream::DrainCiphertext(std::span<std::byte> bytes) -> Result<std::size_t> {
  if (impl_->failed) return TlsError();
  if (bytes.empty()) return std::size_t{0};
  if (impl_->datagram && BIO_ctrl_pending(impl_->transport.get()) > bytes.size())
    return Error{ErrorCode::kMessageTooLarge, "DTLS output buffer would truncate datagram"};
  const auto size =
      (std::min)(bytes.size(), static_cast<std::size_t>((std::numeric_limits<int>::max)()));
  const auto result = BIO_read(impl_->transport.get(), bytes.data(), static_cast<int>(size));
  if (result > 0) return static_cast<std::size_t>(result);
  return Error{ErrorCode::kWouldBlock, "TLS ciphertext output buffer empty"};
}

auto TlsStream::Read(std::span<std::byte> bytes) -> Result<std::size_t> {
  if (impl_->failed) return TlsError();
  if (impl_->datagram && bytes.size() < DatagramPayloadMtu())
    return Error{ErrorCode::kInvalidArgument, "DTLS read buffer must hold a complete payload"};
  ERR_clear_error();
  std::size_t count = 0;
  const auto result = SSL_read_ex(impl_->ssl.get(), bytes.data(), bytes.size(), &count);
  if (result == 1) return count;
  return impl_->Check(result);
}

auto TlsStream::Write(std::span<const std::byte> bytes) -> Result<std::size_t> {
  if (impl_->failed) return TlsError();
  if (!IsReady()) return Error{ErrorCode::kWouldBlock, "TLS handshake not complete"};
  if (impl_->datagram && bytes.size() > DatagramPayloadMtu())
    return Error{ErrorCode::kMessageTooLarge, "DTLS payload exceeds negotiated data MTU"};
  ERR_clear_error();
  std::size_t count = 0;
  const auto result = SSL_write_ex(impl_->ssl.get(), bytes.data(), bytes.size(), &count);
  if (result == 1) return count;
  return impl_->Check(result);
}

auto TlsStream::RetransmitTimeout() const -> std::optional<Duration> {
  if (impl_->failed || !impl_->datagram) return std::nullopt;
  timeval timeout{};
  if (DTLSv1_get_timeout(impl_->ssl.get(), &timeout) != 1) return std::nullopt;
  return std::chrono::seconds(timeout.tv_sec) + std::chrono::microseconds(timeout.tv_usec);
}

auto TlsStream::HandleTimeout() -> Result<void> {
  if (impl_->failed) return TlsError();
  if (!impl_->datagram)
    return Error{ErrorCode::kNotSupported, "DTLS timer requires datagram transport"};
  ERR_clear_error();
  if (DTLSv1_handle_timeout(impl_->ssl.get()) < 0) {
    impl_->failed = true;
    return TlsError();
  }
  return {};
}

auto TlsStream::DatagramPayloadMtu() const -> std::size_t {
  return impl_->datagram && IsReady() ? DTLS_get_data_mtu(impl_->ssl.get()) : 0;
}

}  // namespace atlas
