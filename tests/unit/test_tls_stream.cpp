#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <openssl/ssl.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#if ATLAS_BUILD_NET_CLIENT
#include "baseapp/baseapp_messages.h"
#include "loginapp/login_messages.h"
#include "net_client/client_session.h"
#endif
#include "network/event_dispatcher.h"
#include "network/network_interface.h"
#include "network/reliable_udp.h"
#include "network/tls_stream.h"
#include "platform/environment.h"
#include "server/server_config.h"

namespace atlas {
namespace {

struct EncryptedEcho {
  std::string text;
  static auto Descriptor() -> const MessageDesc& {
    static const MessageDesc desc{510,
                                  "EncryptedEcho",
                                  MessageLengthStyle::kVariable,
                                  -1,
                                  MessageReliability::kReliable,
                                  MessageUrgency::kImmediate};
    return desc;
  }
  void Serialize(BinaryWriter& writer) const { writer.WriteString(text); }
  static auto Deserialize(BinaryReader& reader) -> Result<EncryptedEcho> {
    auto text = reader.ReadString();
    if (!text) return text.Error();
    return EncryptedEcho{std::move(*text)};
  }
};

class TlsStreamTest : public ::testing::Test {
 protected:
  void SetUp() override {
    auto directory = ReadEnvironmentVariable("ATLAS_TEST_TLS_DIR");
    ASSERT_TRUE(directory);
    ASSERT_FALSE(directory->empty());
    directory_ = *directory;
  }
  auto Path(const char* name) -> std::string { return (directory_ / name).string(); }
  auto Pair(const char* trust = "ca.pem", const char* identity = "atlas.test",
            const char* certificate = "server.pem", TlsTransport transport = TlsTransport::kStream)
      -> bool {
    auto server_context = TlsContext::Server(Path(certificate), Path("server.key"), transport);
    auto client_context = TlsContext::Client(Path(trust), transport);
    if (!server_context || !client_context) return false;
    auto server = TlsStream::Create(*server_context);
    auto client = TlsStream::Create(*client_context, identity);
    if (!server || !client) return false;
    server_ = std::move(*server);
    client_ = std::move(*client);
    return true;
  }
  auto Transfer(TlsStream& source, TlsStream& target, std::size_t fragment = 4096) -> bool {
    std::array<std::byte, 4096> buffer{};
    for (int i = 0; i < 128; ++i) {
      auto read = source.DrainCiphertext(std::span(buffer).first(fragment));
      if (!read) return read.Error().Code() == ErrorCode::kWouldBlock;
      auto written = target.FeedCiphertext(std::span(buffer).first(*read));
      if (!written || *written != *read) return false;
    }
    return false;
  }
  auto Handshake(std::size_t fragment = 4096) -> bool {
    for (int i = 0; i < 128; ++i) {
      auto client = client_->Handshake();
      if (!client || !Transfer(*client_, *server_, fragment)) return false;
      auto server = server_->Handshake();
      if (!server || !Transfer(*server_, *client_, fragment)) return false;
      if (*client && *server) return true;
    }
    return false;
  }
  std::filesystem::path directory_;
  std::unique_ptr<TlsStream> server_;
  std::unique_ptr<TlsStream> client_;
};

TEST_F(TlsStreamTest, TrustedDnsAndIpIdentityExchangeEncryptedData) {
  for (const char* identity : {"atlas.test", "127.0.0.1"}) {
    ASSERT_TRUE(Pair("ca.pem", identity));
    ASSERT_TRUE(Handshake(257));
    ASSERT_TRUE(client_->IsReady());
    ASSERT_TRUE(server_->IsReady());
    const std::string plaintext = "Atlas test-only secret payload";
    auto bytes = std::as_bytes(std::span(plaintext));
    auto written = client_->Write(bytes);
    ASSERT_TRUE(written);
    EXPECT_EQ(*written, plaintext.size());
    std::array<std::byte, 4096> wire{};
    auto count = client_->DrainCiphertext(wire);
    ASSERT_TRUE(count);
    EXPECT_GT(*count, plaintext.size());
    EXPECT_EQ(std::search(wire.begin(), wire.begin() + *count, bytes.begin(), bytes.end()),
              wire.begin() + *count);
    ASSERT_TRUE(server_->FeedCiphertext(std::span(wire).first(*count)));
    std::array<std::byte, 128> received{};
    auto read = server_->Read(received);
    ASSERT_TRUE(read);
    EXPECT_EQ(*read, bytes.size());
    EXPECT_TRUE(std::equal(bytes.begin(), bytes.end(), received.begin()));
    ASSERT_TRUE(server_->Write(bytes));
    ASSERT_TRUE(Transfer(*server_, *client_));
    read = client_->Read(received);
    ASSERT_TRUE(read);
    EXPECT_EQ(*read, bytes.size());
    EXPECT_TRUE(std::equal(bytes.begin(), bytes.end(), received.begin()));
  }
}

TEST_F(TlsStreamTest, RejectsWrongIdentityUntrustedIssuerAndExpiredCertificate) {
  ASSERT_TRUE(Pair("ca.pem", "wrong.test"));
  EXPECT_FALSE(Handshake());
  EXPECT_FALSE(client_->IsReady());
  ASSERT_TRUE(Pair("other.pem"));
  EXPECT_FALSE(Handshake());
  EXPECT_FALSE(client_->IsReady());
  ASSERT_TRUE(Pair("ca.pem", "atlas.test", "expired.pem"));
  EXPECT_FALSE(Handshake());
  EXPECT_FALSE(client_->IsReady());
}

TEST_F(TlsStreamTest, RejectsMissingInputsAndMismatchedPrivateKey) {
  EXPECT_FALSE(TlsContext::Server("", Path("server.key")));
  EXPECT_FALSE(TlsContext::Server(Path("server.pem"), Path("other.key")));
  EXPECT_FALSE(TlsContext::Server(Path("server.pem"), Path("encrypted.key")));
  EXPECT_FALSE(TlsContext::Client(Path("absent.pem")));
  EXPECT_FALSE(TlsContext::Client(""));
  auto context = TlsContext::Client(Path("ca.pem"));
  ASSERT_TRUE(context);
  EXPECT_FALSE(TlsStream::Create(*context));
  EXPECT_FALSE(TlsStream::Create(*context, std::string("atlas.test\0other", 16)));
  EXPECT_FALSE(TlsStream::Create(nullptr));
}

TEST_F(TlsStreamTest, TamperedRecordFailsPermanentlyBeforeApplicationDelivery) {
  ASSERT_TRUE(Pair());
  ASSERT_TRUE(Handshake());
  const std::string text = "test-only payload";
  ASSERT_TRUE(client_->Write(std::as_bytes(std::span(text))));
  std::array<std::byte, 4096> wire{};
  auto count = client_->DrainCiphertext(wire);
  ASSERT_TRUE(count);
  wire[*count - 1] ^= std::byte{1};
  ASSERT_TRUE(server_->FeedCiphertext(std::span(wire).first(*count)));
  std::array<std::byte, 128> output{};
  auto read = server_->Read(output);
  ASSERT_FALSE(read);
  EXPECT_EQ(read.Error().Code(), ErrorCode::kPermissionDenied);
  EXPECT_FALSE(server_->IsReady());
  EXPECT_FALSE(server_->Handshake());
  EXPECT_FALSE(server_->Read(output));
  EXPECT_FALSE(server_->Write(std::as_bytes(std::span(text))));
}

TEST_F(TlsStreamTest, IncompleteRecordDoesNotDeliverApplicationData) {
  ASSERT_TRUE(Pair());
  const std::string text = "complete record required";
  EXPECT_FALSE(client_->Write(std::as_bytes(std::span(text))));
  ASSERT_TRUE(Handshake());
  ASSERT_TRUE(client_->Write(std::as_bytes(std::span(text))));
  std::array<std::byte, 4096> wire{};
  auto count = client_->DrainCiphertext(wire);
  ASSERT_TRUE(count);
  ASSERT_GT(*count, 1u);
  ASSERT_TRUE(server_->FeedCiphertext(std::span(wire).first(*count - 1)));
  std::array<std::byte, 128> output{};
  auto read = server_->Read(output);
  ASSERT_FALSE(read);
  EXPECT_EQ(read.Error().Code(), ErrorCode::kWouldBlock);
  ASSERT_TRUE(server_->FeedCiphertext(std::span(wire).subspan(*count - 1, 1)));
  read = server_->Read(output);
  ASSERT_TRUE(read);
  EXPECT_EQ(*read, text.size());
}

TEST_F(TlsStreamTest, RejectsTls12ClientWithoutDowngrade) {
  ASSERT_TRUE(Pair());
  std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)> context(SSL_CTX_new(TLS_client_method()),
                                                            SSL_CTX_free);
  ASSERT_NE(context, nullptr);
  ASSERT_EQ(SSL_CTX_set_max_proto_version(context.get(), TLS1_2_VERSION), 1);
  std::unique_ptr<SSL, decltype(&SSL_free)> client(SSL_new(context.get()), SSL_free);
  ASSERT_NE(client, nullptr);
  BIO* internal = nullptr;
  BIO* transport = nullptr;
  ASSERT_EQ(BIO_new_bio_pair(&internal, 65536, &transport, 65536), 1);
  std::unique_ptr<BIO, decltype(&BIO_free)> wire(transport, BIO_free);
  SSL_set_bio(client.get(), internal, internal);
  SSL_set_connect_state(client.get());
  EXPECT_NE(SSL_do_handshake(client.get()), 1);
  std::array<std::byte, 4096> hello{};
  const auto count = BIO_read(wire.get(), hello.data(), static_cast<int>(hello.size()));
  ASSERT_GT(count, 0);
  ASSERT_TRUE(server_->FeedCiphertext(std::span(hello).first(static_cast<std::size_t>(count))));
  EXPECT_FALSE(server_->Handshake());
  EXPECT_FALSE(server_->IsReady());
}

TEST_F(TlsStreamTest, CiphertextInputAndOutputHaveBoundedBackpressure) {
  ASSERT_TRUE(Pair());
  std::vector<std::byte> oversized(2 * TlsStream::kCipherBufferSize);
  auto accepted = server_->FeedCiphertext(oversized);
  ASSERT_TRUE(accepted);
  EXPECT_EQ(*accepted, TlsStream::kCipherBufferSize);
  auto full = server_->FeedCiphertext(oversized);
  ASSERT_FALSE(full);
  EXPECT_EQ(full.Error().Code(), ErrorCode::kWouldBlock);
  ASSERT_TRUE(Pair());
  ASSERT_TRUE(Handshake());
  std::array<std::byte, 16384> payload{};
  bool blocked = false;
  for (int i = 0; i < 16; ++i) {
    auto written = client_->Write(payload);
    if (!written) {
      EXPECT_EQ(written.Error().Code(), ErrorCode::kWouldBlock);
      blocked = true;
      break;
    }
  }
  EXPECT_TRUE(blocked);
}

TEST_F(TlsStreamTest, DatagramPreservesBoundariesAndRejectsReplayAndTampering) {
  ASSERT_TRUE(Pair("ca.pem", "atlas.test", "server.pem", TlsTransport::kDatagram));
  ASSERT_TRUE(Handshake());
  ASSERT_GT(client_->DatagramPayloadMtu(), 470u);
  ASSERT_LT(client_->DatagramPayloadMtu(), 1200u);
  const std::array<std::byte, 3> message{std::byte{1}, std::byte{2}, std::byte{3}};
  ASSERT_TRUE(client_->Write(message));
  std::array<std::byte, 4096> cipher{};
  auto count = client_->DrainCiphertext(cipher);
  ASSERT_TRUE(count);
  ASSERT_TRUE(server_->FeedCiphertext(std::span(cipher).first(*count)));
  std::array<std::byte, 4096> plain{};
  auto received = server_->Read(plain);
  ASSERT_TRUE(received);
  EXPECT_EQ(*received, message.size());
  EXPECT_TRUE(std::equal(message.begin(), message.end(), plain.begin()));
  ASSERT_TRUE(server_->FeedCiphertext(std::span(cipher).first(*count)));
  received = server_->Read(plain);
  ASSERT_FALSE(received);
  EXPECT_EQ(received.Error().Code(), ErrorCode::kWouldBlock);
  ASSERT_TRUE(client_->Write(message));
  count = client_->DrainCiphertext(cipher);
  ASSERT_TRUE(count);
  cipher[*count - 1] ^= std::byte{1};
  ASSERT_TRUE(server_->FeedCiphertext(std::span(cipher).first(*count)));
  received = server_->Read(plain);
  ASSERT_FALSE(received);
  EXPECT_EQ(received.Error().Code(), ErrorCode::kWouldBlock);
  EXPECT_TRUE(server_->IsReady());
  ASSERT_TRUE(client_->Write(message));
  ASSERT_TRUE(Transfer(*client_, *server_));
  received = server_->Read(plain);
  ASSERT_TRUE(received);
  EXPECT_EQ(*received, message.size());
}

TEST_F(TlsStreamTest, DatagramRejectsWrongCertificateIdentityAndIssuer) {
  for (const auto* identity : {"wrong.test", "127.0.0.2"}) {
    ASSERT_TRUE(Pair("ca.pem", identity, "server.pem", TlsTransport::kDatagram));
    EXPECT_FALSE(Handshake());
  }
  ASSERT_TRUE(Pair("other.pem", "atlas.test", "server.pem", TlsTransport::kDatagram));
  EXPECT_FALSE(Handshake());
  ASSERT_TRUE(Pair("ca.pem", "atlas.test", "expired.pem", TlsTransport::kDatagram));
  EXPECT_FALSE(Handshake());
}

TEST_F(TlsStreamTest, DatagramRejectsOversizeAndTruncationWithoutConsumingRecord) {
  ASSERT_TRUE(Pair("ca.pem", "127.0.0.1", "server.pem", TlsTransport::kDatagram));
  ASSERT_TRUE(Handshake());
  std::vector<std::byte> oversized(1201);
  EXPECT_FALSE(server_->FeedCiphertext(oversized));
  EXPECT_FALSE(client_->Write(oversized));
  const std::array<std::byte, 3> message{};
  ASSERT_TRUE(client_->Write(message));
  std::array<std::byte, 1> too_small{};
  auto truncated = client_->DrainCiphertext(too_small);
  ASSERT_FALSE(truncated);
  EXPECT_EQ(truncated.Error().Code(), ErrorCode::kMessageTooLarge);
  ASSERT_TRUE(Transfer(*client_, *server_));
  EXPECT_FALSE(server_->Read(too_small));
  std::array<std::byte, 4096> plain{};
  auto received = server_->Read(plain);
  ASSERT_TRUE(received);
  EXPECT_EQ(*received, message.size());
}

TEST_F(TlsStreamTest, DatagramRetransmitsLostHandshakeUsingDispatcherTimer) {
  ASSERT_TRUE(Pair("ca.pem", "atlas.test", "server.pem", TlsTransport::kDatagram));
  auto first = client_->Handshake();
  ASSERT_TRUE(first);
  EXPECT_FALSE(*first);
  std::array<std::byte, 4096> lost{};
  ASSERT_TRUE(client_->DrainCiphertext(lost));
  auto timeout = client_->RetransmitTimeout();
  ASSERT_TRUE(timeout);
  EventDispatcher dispatcher("dtls-retransmit");
  dispatcher.SetMaxPollWait(Milliseconds(1));
  bool fired = false;
  const auto timer = dispatcher.AddTimer(*timeout + Milliseconds(10), [&](TimerHandle) {
    fired = true;
    EXPECT_TRUE(client_->HandleTimeout());
  });
  ASSERT_TRUE(timer.IsValid());
  const auto deadline = Clock::now() + Seconds(3);
  while (!fired && Clock::now() < deadline) dispatcher.ProcessOnce();
  ASSERT_TRUE(fired);
  ASSERT_TRUE(Transfer(*client_, *server_));
  ASSERT_TRUE(Handshake());
}

TEST_F(TlsStreamTest, DatagramAcceptsReorderingAndDoesNotCoalesceMessages) {
  ASSERT_TRUE(Pair("ca.pem", "atlas.test", "server.pem", TlsTransport::kDatagram));
  ASSERT_TRUE(Handshake());
  const std::array<std::byte, 1> first{std::byte{1}};
  const std::array<std::byte, 2> second{std::byte{2}, std::byte{3}};
  ASSERT_TRUE(client_->Write(first));
  ASSERT_TRUE(client_->Write(second));
  std::array<std::byte, 4096> cipher_first{}, cipher_second{}, plain{};
  auto first_count = client_->DrainCiphertext(cipher_first);
  auto second_count = client_->DrainCiphertext(cipher_second);
  ASSERT_TRUE(first_count);
  ASSERT_TRUE(second_count);
  ASSERT_TRUE(server_->FeedCiphertext(std::span(cipher_second).first(*second_count)));
  ASSERT_TRUE(server_->FeedCiphertext(std::span(cipher_first).first(*first_count)));
  auto received = server_->Read(plain);
  ASSERT_TRUE(received);
  EXPECT_EQ(*received, second.size());
  EXPECT_TRUE(std::equal(second.begin(), second.end(), plain.begin()));
  received = server_->Read(plain);
  ASSERT_TRUE(received);
  EXPECT_EQ(*received, first.size());
  EXPECT_EQ(plain[0], first[0]);
}

TEST_F(TlsStreamTest, SharedUdpDtlsRoundTripIncludesRudpFragmentsRetransmissionAndAcks) {
  auto server_context =
      TlsContext::Server(Path("server.pem"), Path("server.key"), TlsTransport::kDatagram);
  auto client_context = TlsContext::Client(Path("ca.pem"), TlsTransport::kDatagram);
  ASSERT_TRUE(server_context);
  ASSERT_TRUE(client_context);
  EventDispatcher dispatcher("encrypted-rudp");
  dispatcher.SetMaxPollWait(Milliseconds(1));
  NetworkInterface server(dispatcher), client(dispatcher);
  ASSERT_TRUE(server.ConfigureDtls(*server_context));
  ASSERT_TRUE(client.ConfigureDtls(*client_context, "atlas.test"));
  const auto profile = NetworkInterface::InternetRudpProfile();
  ASSERT_TRUE(server.StartRudpServer(Address("127.0.0.1", 0), profile));
  auto connected = client.ConnectRudp(server.RudpAddress(), profile);
  ASSERT_TRUE(connected);
  auto early = (*connected)->SendMessage(EncryptedEcho{"before-handshake"});
  ASSERT_FALSE(early);
  EXPECT_EQ(early.Error().Code(), ErrorCode::kWouldBlock);
  const auto handshake_deadline = Clock::now() + Seconds(3);
  while (!client.IsRudpReady(server.RudpAddress()) && Clock::now() < handshake_deadline)
    dispatcher.ProcessOnce();
  ASSERT_TRUE(client.IsRudpReady(server.RudpAddress()));
  EXPECT_EQ(server.ChannelCount(), 0u);
  server.SetAcceptCallback([](Channel& channel) {
    auto& rudp = static_cast<ReliableUdpChannel&>(channel);
    rudp.SetInboundDropWindow(Clock::now(), 0, 120);
    rudp.SetTransportImpairment(5, 0, 7);
  });
  std::string received;
  int deliveries = 0;
  bool closed_in_callback = false;
  ASSERT_TRUE(server.InterfaceTable().RegisterTypedHandler<EncryptedEcho>(
      [](const Address&, Channel* channel, const EncryptedEcho& message) {
        EXPECT_TRUE(channel->SendMessage(message));
      }));
  ASSERT_TRUE(client.InterfaceTable().RegisterTypedHandler<EncryptedEcho>(
      [&](const Address&, Channel*, const EncryptedEcho& message) {
        received = message.text;
        ++deliveries;
        if (message.text == "close-in-callback") {
          client.DisconnectChannel(server.RudpAddress());
          client.CloseRudpSocket();
          closed_in_callback = true;
        }
      }));
  const EncryptedEcho payload{std::string(10000, 'x')};
  ASSERT_TRUE((*connected)->SendMessage(payload));
  const auto deadline = Clock::now() + Seconds(5);
  while (
      (received.empty() || (*connected)->UnackedCount() != 0 || server.PendingSendCount() != 0) &&
      Clock::now() < deadline)
    dispatcher.ProcessOnce();
  EXPECT_EQ(received, payload.text);
  EXPECT_EQ(deliveries, 1);
  EXPECT_EQ((*connected)->UnackedCount(), 0u);
  EXPECT_EQ(server.PendingSendCount(), 0u);
  ASSERT_TRUE((*connected)->SendMessage(EncryptedEcho{"close-in-callback"}));
  const auto close_deadline = Clock::now() + Seconds(3);
  while (!closed_in_callback && Clock::now() < close_deadline) dispatcher.ProcessOnce();
  EXPECT_TRUE(closed_in_callback);
  EXPECT_EQ(client.DtlsSessionCount(), 0u);
}

TEST_F(TlsStreamTest, SharedUdpDtlsRejectsPlaintextAndWrongTargetIdentity) {
  auto server_context =
      TlsContext::Server(Path("server.pem"), Path("server.key"), TlsTransport::kDatagram);
  auto client_context = TlsContext::Client(Path("ca.pem"), TlsTransport::kDatagram);
  ASSERT_TRUE(server_context);
  ASSERT_TRUE(client_context);
  EventDispatcher dispatcher("dtls-rejections");
  dispatcher.SetMaxPollWait(Milliseconds(1));
  NetworkInterface server(dispatcher), plain(dispatcher), wrong(dispatcher);
  ASSERT_TRUE(server.ConfigureDtls(*server_context));
  ASSERT_TRUE(
      server.StartRudpServer(Address("127.0.0.1", 0), NetworkInterface::InternetRudpProfile()));
  auto insecure = plain.ConnectRudp(server.RudpAddress(), NetworkInterface::InternetRudpProfile());
  ASSERT_TRUE(insecure);
  ASSERT_TRUE((*insecure)->SendMessage(EncryptedEcho{"plaintext-rejected"}));
  ASSERT_TRUE(wrong.ConfigureDtls(*client_context, "wrong.test"));
  ASSERT_TRUE(wrong.ConnectRudp(server.RudpAddress(), NetworkInterface::InternetRudpProfile()));
  bool disconnected = false;
  wrong.SetDisconnectCallback([&](Channel&) { disconnected = true; });
  const auto deadline = Clock::now() + Seconds(3);
  while (!disconnected && Clock::now() < deadline) dispatcher.ProcessOnce();
  EXPECT_TRUE(disconnected);
  EXPECT_EQ(server.ChannelCount(), 0u);
  EXPECT_FALSE(wrong.IsRudpReady(server.RudpAddress()));
  EXPECT_EQ(wrong.DtlsSessionCount(), 0u);
}

TEST_F(TlsStreamTest, DtlsConfigurationIsExplicitAndCannotChangeLiveTransport) {
  EventDispatcher dispatcher("dtls-config");
  NetworkInterface network(dispatcher);
  EXPECT_FALSE(network.ConfigureDtls(nullptr));
  auto stream = TlsContext::Client(Path("ca.pem"));
  ASSERT_TRUE(stream);
  EXPECT_FALSE(network.ConfigureDtls(*stream, "atlas.test"));
  auto datagram = TlsContext::Client(Path("ca.pem"), TlsTransport::kDatagram);
  ASSERT_TRUE(datagram);
  EXPECT_FALSE(network.ConfigureDtls(*datagram));
  ASSERT_TRUE(network.ConfigureDtls(*datagram, "atlas.test"));
  EXPECT_FALSE(network.ConfigureDtls(*datagram, "other.test"));
  EXPECT_FALSE(network.StartRudpServer(Address("127.0.0.1", 0)));
  NetworkInterface listener(dispatcher);
  auto server = TlsContext::Server(Path("server.pem"), Path("server.key"), TlsTransport::kDatagram);
  ASSERT_TRUE(server);
  ASSERT_TRUE(listener.ConfigureDtls(*server));
  EXPECT_FALSE(listener.ConnectRudp(Address("127.0.0.1", 9)));
  EXPECT_EQ(listener.RudpAddress(), Address{});
  EXPECT_TRUE(
      listener.StartRudpServer(Address("127.0.0.1", 0), NetworkInterface::InternetRudpProfile()));
}

TEST_F(TlsStreamTest, SharedUdpDtlsBoundsHandshakeAdmissionAndExpiresUnfinishedPeers) {
  auto server_context =
      TlsContext::Server(Path("server.pem"), Path("server.key"), TlsTransport::kDatagram);
  auto client_context = TlsContext::Client(Path("ca.pem"), TlsTransport::kDatagram);
  ASSERT_TRUE(server_context);
  ASSERT_TRUE(client_context);
  auto client = TlsStream::Create(*client_context, "atlas.test");
  ASSERT_TRUE(client);
  ASSERT_TRUE((*client)->Handshake());
  std::array<std::byte, 1200> hello{};
  auto size = (*client)->DrainCiphertext(hello);
  ASSERT_TRUE(size);
  EventDispatcher dispatcher("dtls-admission");
  dispatcher.SetMaxPollWait(Milliseconds(1));
  NetworkInterface server(dispatcher);
  ASSERT_TRUE(server.ConfigureDtls(*server_context));
  ASSERT_TRUE(
      server.StartRudpServer(Address("127.0.0.1", 0), NetworkInterface::InternetRudpProfile()));
  std::vector<Socket> peers;
  std::unordered_set<Address> sources;
  for (int i = 0; i < 130; ++i) {
    SCOPED_TRACE(i);
    auto peer = Socket::CreateUdp();
    ASSERT_TRUE(peer);
    ASSERT_TRUE(peer->SetReuseAddr(false));
    ASSERT_TRUE(peer->Bind(Address("127.0.0.1", 0)));
    auto source = peer->LocalAddress();
    ASSERT_TRUE(source);
    ASSERT_TRUE(sources.insert(*source).second);
    ASSERT_TRUE(peer->SetNonBlocking(true));
    auto verified_client = TlsStream::Create(*client_context, "atlas.test");
    ASSERT_TRUE(verified_client);
    ASSERT_TRUE((*verified_client)->Handshake());
    std::array<std::byte, 1200> initial{};
    auto initial_size = (*verified_client)->DrainCiphertext(initial);
    ASSERT_TRUE(initial_size);
    ASSERT_TRUE(peer->SendTo(std::span(initial).first(*initial_size), server.RudpAddress()));
    std::array<std::byte, 1200> challenge{};
    std::optional<std::size_t> challenge_size;
    const auto challenge_deadline = Clock::now() + Seconds(2);
    while (!challenge_size && Clock::now() < challenge_deadline) {
      dispatcher.ProcessOnce();
      auto received = peer->RecvFrom(challenge);
      if (received) challenge_size = received->first;
    }
    ASSERT_TRUE(challenge_size);
    ASSERT_GT(*challenge_size, 13u);
    ASSERT_EQ(challenge[13], std::byte{3});
    EXPECT_LE(*challenge_size, *size);
    EXPECT_EQ(server.DtlsSessionCount(), static_cast<std::size_t>((std::min)(i, 128)));
    ASSERT_TRUE((*verified_client)->FeedCiphertext(std::span(challenge).first(*challenge_size)));
    ASSERT_TRUE((*verified_client)->Handshake());
    auto cookie_hello = (*verified_client)->DrainCiphertext(initial);
    ASSERT_TRUE(cookie_hello);
    ASSERT_TRUE(peer->SendTo(std::span(initial).first(*cookie_hello), server.RudpAddress()));
    dispatcher.ProcessOnce();
    peers.push_back(std::move(*peer));
  }
  const auto accepted_deadline = Clock::now() + Seconds(2);
  while (server.DtlsSessionCount() < 128 && Clock::now() < accepted_deadline)
    dispatcher.ProcessOnce();
  EXPECT_EQ(server.DtlsSessionCount(), 128u);
  EXPECT_EQ(server.ChannelCount(), 0u);
  const auto expired_deadline = Clock::now() + Seconds(11);
  while (server.DtlsSessionCount() != 0 && Clock::now() < expired_deadline)
    dispatcher.ProcessOnce();
  EXPECT_EQ(server.DtlsSessionCount(), 0u);
  EXPECT_EQ(server.PendingSendCount(), 0u);
}

TEST_F(TlsStreamTest, StatelessCookieBindsSourceAndRejectsTamperingAndAnotherContext) {
  auto server = TlsContext::Server(Path("server.pem"), Path("server.key"), TlsTransport::kDatagram);
  auto client = TlsContext::Client(Path("ca.pem"), TlsTransport::kDatagram);
  ASSERT_TRUE(server);
  ASSERT_TRUE(client);
  auto initiator = TlsStream::Create(*client, "atlas.test");
  auto listener = TlsStream::Create(*server);
  ASSERT_TRUE(initiator);
  ASSERT_TRUE(listener);
  ASSERT_TRUE((*initiator)->Handshake());
  std::array<std::byte, 1200> wire{};
  auto hello = (*initiator)->DrainCiphertext(wire);
  ASSERT_TRUE(hello);
  ASSERT_TRUE((*listener)->FeedCiphertext(std::span(wire).first(*hello)));
  const Address source("127.0.0.1", 31001);
  auto challenged = (*listener)->ListenDatagram(source);
  ASSERT_TRUE(challenged);
  EXPECT_FALSE(*challenged);
  auto challenge = (*listener)->DrainCiphertext(wire);
  ASSERT_TRUE(challenge);
  EXPECT_LE(*challenge, *hello);
  ASSERT_TRUE((*initiator)->FeedCiphertext(std::span(wire).first(*challenge)));
  ASSERT_TRUE((*initiator)->Handshake());
  auto cookie_hello = (*initiator)->DrainCiphertext(wire);
  ASSERT_TRUE(cookie_hello);
  const auto packet = std::span(wire).first(*cookie_hello);
  const std::vector<std::byte> saved_packet(packet.begin(), packet.end());
  auto verify = [&](const std::shared_ptr<TlsContext>& context, const Address& address,
                    std::span<const std::byte> input) -> bool {
    auto probe = TlsStream::Create(context);
    if (!probe || !(*probe)->FeedCiphertext(input)) return false;
    auto result = (*probe)->ListenDatagram(address);
    return result && *result;
  };
  EXPECT_TRUE(verify(*server, source, packet));
  EXPECT_FALSE(verify(*server, Address("127.0.0.1", 31002), packet));
  EXPECT_FALSE(verify(*server, Address("127.0.0.2", 31001), packet));
  auto other = TlsContext::Server(Path("server.pem"), Path("server.key"), TlsTransport::kDatagram);
  ASSERT_TRUE(other);
  EXPECT_FALSE(verify(*other, source, packet));
  constexpr std::size_t kSessionLengthOffset = 13 + 12 + 2 + 32;
  ASSERT_GT(packet.size(), kSessionLengthOffset);
  const auto cookie_length =
      kSessionLengthOffset + 1 + std::to_integer<std::size_t>(packet[kSessionLengthOffset]);
  ASSERT_GT(packet.size(), cookie_length + 40);
  ASSERT_EQ(packet[cookie_length], std::byte{40});
  wire[cookie_length + 40] ^= std::byte{1};
  EXPECT_FALSE(verify(*server, source, packet));
  wire[cookie_length + 40] ^= std::byte{1};
  std::fill_n(wire.begin() + cookie_length + 1, 8, std::byte{0xff});
  EXPECT_FALSE(verify(*server, source, packet));
  std::this_thread::sleep_for(Seconds(61));
  EXPECT_FALSE(verify(*server, source, saved_packet));
}

TEST_F(TlsStreamTest, PublicProductionRequiresValidDtlsCredentials) {
  EventDispatcher dispatcher("public-config");
  NetworkInterface network(dispatcher);
  ServerConfig config;
  config.is_production = true;
  EXPECT_FALSE(config.ConfigureExternalTransport(network));
  config.external_dtls_certificate = Path("server.pem");
  EXPECT_FALSE(config.ConfigureExternalTransport(network));
  config.external_dtls_private_key = Path("other.key");
  EXPECT_FALSE(config.ConfigureExternalTransport(network));
  config.external_dtls_private_key = Path("server.key");
  ASSERT_TRUE(config.ConfigureExternalTransport(network));
  ASSERT_TRUE(
      network.StartRudpServer(Address("127.0.0.1", 0), NetworkInterface::InternetRudpProfile()));
}

#if ATLAS_BUILD_NET_CLIENT
TEST_F(TlsStreamTest, NativeClientWaitsForLoginAndBaseAuthenticationHandshakes) {
  EventDispatcher dispatcher("sdk-dtls");
  dispatcher.SetMaxPollWait(Milliseconds(1));
  NetworkInterface login_server(dispatcher), base_server(dispatcher);
  ServerConfig config;
  config.is_production = true;
  config.external_dtls_certificate = Path("server.pem");
  config.external_dtls_private_key = Path("server.key");
  ASSERT_TRUE(config.ConfigureExternalTransport(login_server));
  ASSERT_TRUE(config.ConfigureExternalTransport(base_server));
  const auto profile = NetworkInterface::InternetRudpProfile();
  ASSERT_TRUE(login_server.StartRudpServer(Address("127.0.0.1", 0), profile));
  ASSERT_TRUE(base_server.StartRudpServer(Address("127.0.0.1", 0), profile));
  int logins = 0;
  ASSERT_TRUE(login_server.InterfaceTable().RegisterTypedHandler<login::LoginRequest>(
      [&](const Address&, Channel* channel, const login::LoginRequest& request) {
        EXPECT_EQ(request.password_hash, "test-only-credential");
        ++logins;
        login::LoginResult reply;
        reply.status = login::LoginStatus::kSuccess;
        reply.baseapp_addr = base_server.RudpAddress();
        reply.session_key.bytes[0] = 7;
        EXPECT_TRUE(channel->SendMessage(reply));
      }));
  ASSERT_TRUE(base_server.InterfaceTable().RegisterTypedHandler<baseapp::Authenticate>(
      [&](const Address&, Channel* channel, const baseapp::Authenticate& request) {
        EXPECT_EQ(request.session_key.bytes[0], 7);
        baseapp::AuthenticateResult reply;
        reply.success = true;
        reply.entity_id = 21;
        reply.type_id = 1;
        EXPECT_TRUE(channel->SendMessage(reply));
      }));
  net_client::ClientSession client;
  client.SetCallbacks({});
  ASSERT_EQ(client.ConfigureDtls(Path("ca.pem").c_str(), "atlas.test"), ATLAS_NET_OK);
  bool logged_in = false;
  auto login_callback = [](void* data, uint8_t status, const char*, uint16_t, const char*) {
    EXPECT_EQ(status, ATLAS_LOGIN_SUCCESS);
    *static_cast<bool*>(data) = true;
  };
  ASSERT_EQ(client.StartLogin("127.0.0.1", login_server.RudpAddress().Port(), "sdk_account",
                              "test-only-credential", login_callback, &logged_in),
            ATLAS_NET_OK);
  EXPECT_EQ(logins, 0);
  auto deadline = Clock::now() + Seconds(3);
  while (!logged_in && Clock::now() < deadline) {
    dispatcher.ProcessOnce();
    client.Poll();
  }
  ASSERT_TRUE(logged_in);
  EXPECT_EQ(logins, 1);
  bool authenticated = false;
  auto auth_callback = [](void* data, uint8_t success, uint32_t entity, uint16_t, const char*) {
    EXPECT_EQ(success, 1);
    EXPECT_EQ(entity, 21u);
    *static_cast<bool*>(data) = true;
  };
  ASSERT_EQ(client.StartAuthenticate(auth_callback, &authenticated), ATLAS_NET_OK);
  deadline = Clock::now() + Seconds(3);
  while (!authenticated && Clock::now() < deadline) {
    dispatcher.ProcessOnce();
    client.Poll();
  }
  EXPECT_TRUE(authenticated);
}

TEST_F(TlsStreamTest, FailedExplicitDtlsConfigurationCannotFallBackToPlaintext) {
  net_client::ClientSession client;
  client.SetCallbacks({});
  EXPECT_NE(client.ConfigureDtls(Path("missing.pem").c_str(), "atlas.test"), ATLAS_NET_OK);
  auto callback = [](void*, uint8_t, const char*, uint16_t, const char*) { ADD_FAILURE(); };
  EXPECT_EQ(client.StartLogin("127.0.0.1", 9, "account", "test-only-credential", callback, nullptr),
            ATLAS_NET_ERR_NOCONN);
  EXPECT_NE(client.LastError().find("plaintext"), std::string::npos);
}
#endif

}  // namespace
}  // namespace atlas
