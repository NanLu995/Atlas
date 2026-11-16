#ifndef ATLAS_LIB_NETWORK_TLS_STREAM_H_
#define ATLAS_LIB_NETWORK_TLS_STREAM_H_

#include <memory>
#include <optional>
#include <span>
#include <string>

#include "foundation/clock.h"
#include "foundation/error.h"
#include "network/address.h"

namespace atlas {

class TlsStream;
enum class TlsTransport { kStream, kDatagram };

class TlsContext {
 public:
  ~TlsContext();
  [[nodiscard]] auto IsDatagram() const -> bool;
  [[nodiscard]] auto IsServer() const -> bool;
  [[nodiscard]] static auto Server(const std::string& certificate, const std::string& key,
                                   TlsTransport transport = TlsTransport::kStream)
      -> Result<std::shared_ptr<TlsContext>>;
  [[nodiscard]] static auto Client(const std::string& trust_file,
                                   TlsTransport transport = TlsTransport::kStream)
      -> Result<std::shared_ptr<TlsContext>>;

 private:
  friend class TlsStream;
  struct Impl;
  explicit TlsContext(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

// Dispatcher-thread only; the bounded BIO pair keeps ciphertext outside socket ownership.
class TlsStream {
 public:
  static constexpr std::size_t kCipherBufferSize = 64 * 1024;
  ~TlsStream();
  [[nodiscard]] static auto Create(const std::shared_ptr<TlsContext>& context,
                                   const std::string& peer_identity = "")
      -> Result<std::unique_ptr<TlsStream>>;
  [[nodiscard]] auto Handshake() -> Result<bool>;
  [[nodiscard]] auto ListenDatagram(const Address& peer) -> Result<bool>;
  [[nodiscard]] auto IsReady() const -> bool;
  // Datagram mode feeds and drains whole datagrams; output is never silently truncated.
  [[nodiscard]] auto FeedCiphertext(std::span<const std::byte> bytes) -> Result<std::size_t>;
  [[nodiscard]] auto DrainCiphertext(std::span<std::byte> bytes) -> Result<std::size_t>;
  [[nodiscard]] auto Read(std::span<std::byte> bytes) -> Result<std::size_t>;
  // After WouldBlock, retry the same bytes before writing different application data.
  [[nodiscard]] auto Write(std::span<const std::byte> bytes) -> Result<std::size_t>;
  [[nodiscard]] auto RetransmitTimeout() const -> std::optional<Duration>;
  [[nodiscard]] auto HandleTimeout() -> Result<void>;
  [[nodiscard]] auto DatagramPayloadMtu() const -> std::size_t;

 private:
  struct Impl;
  explicit TlsStream(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace atlas

#endif  // ATLAS_LIB_NETWORK_TLS_STREAM_H_
