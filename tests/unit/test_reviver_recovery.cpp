#include <gtest/gtest.h>

#include "baseappmgr/baseappmgr_messages.h"
#include "cellappmgr/cellappmgr_messages.h"
#include "dbappmgr/dbappmgr_messages.h"
#include "network/event_dispatcher.h"
#include "network/network_interface.h"
#include "network/reliable_udp.h"
#include "reviver/reviver.h"

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace atlas {

template <typename Probe, typename Ack>
void CheckHeartbeatAfterChannelRecreation() {
  EventDispatcher dispatcher{"heartbeat_recreation"};
  dispatcher.SetMaxPollWait(Milliseconds(1));
  NetworkInterface sender{dispatcher};
  NetworkInterface subject{dispatcher};
  ASSERT_TRUE(subject.StartRudpServer(Address("127.0.0.1", 0)));
  ASSERT_TRUE(subject.InterfaceTable().RegisterTypedHandler<Probe>(
      [](const Address&, Channel* channel, const Probe& probe) {
        ASSERT_TRUE(channel->SendMessage(Ack{probe.nonce, 1, true}));
      }));
  uint64_t received = 0;
  ASSERT_TRUE(sender.InterfaceTable().RegisterTypedHandler<Ack>(
      [&](const Address&, Channel*, const Ack& ack) { received = ack.nonce; }));
  const auto address = subject.RudpAddress();
  for (uint64_t nonce : {1u, 2u}) {
    auto channel = sender.ConnectRudpNocwnd(address);
    ASSERT_TRUE(channel);
    ASSERT_TRUE((*channel)->SendMessage(Probe{nonce, 255}));
    const auto deadline = Clock::now() + Seconds(2);
    while (received != nonce && Clock::now() < deadline) {
      sender.FlushDirtySendChannels();
      subject.FlushDirtySendChannels();
      dispatcher.ProcessOnce();
    }
    ASSERT_EQ(received, nonce);
    sender.DisconnectChannel(address);
  }
}

TEST(ReviverRecovery, HeartbeatSurvivesChannelRecreationWithTheSameLocalAddress) {
  CheckHeartbeatAfterChannelRecreation<cellappmgr::HealthProbe, cellappmgr::HealthProbeAck>();
  CheckHeartbeatAfterChannelRecreation<baseappmgr::HealthProbe, baseappmgr::HealthProbeAck>();
  CheckHeartbeatAfterChannelRecreation<dbappmgr::HealthProbe, dbappmgr::HealthProbeAck>();
}

struct ReviverTestAccess {
  static void SetTarget(Reviver& reviver, const Address& address, uint32_t pid) {
    auto& target = reviver.cellappmgr_target_;
    target.last_addr = address;
    target.last_pid = pid;
    target.ever_active = true;
    target.is_active_reviver = true;
    target.slug = "cellappmgr";
  }
  static auto CanRestart(Reviver& reviver) -> bool {
    return reviver.CanRestartTarget(reviver.cellappmgr_target_);
  }
  static void Launch(Reviver& reviver) { reviver.LaunchTarget(reviver.cellappmgr_target_); }
  static auto Blocked(const Reviver& reviver) -> bool {
    return reviver.cellappmgr_target_.restart_blocked;
  }
  static auto Status(const Reviver& reviver) -> std::string {
    return reviver.TargetStatus(reviver.cellappmgr_target_);
  }
  static auto LaunchCount(const Reviver& reviver) -> uint64_t {
    return reviver.cellappmgr_target_.launch_count;
  }
  static auto FailedWatcherWithoutRegistry(Reviver& reviver) -> uint64_t {
    auto& target = reviver.cellappmgr_target_;
    target.active = true;
    target.health_pending = true;
    target.health_check_generation = 1;
    reviver.OnTargetHealth(target, 1, false);
    EXPECT_FALSE(target.health_pending);
    return target.manager_health_failure_count;
  }
  static auto MissingUnconfirmedRegistry(Reviver& reviver) -> uint64_t {
    auto& target = reviver.cellappmgr_target_;
    target.active = true;
    target.missing_audit_threshold = 1;
    reviver.OnTargetRegistryAudit(target, {});
    EXPECT_TRUE(target.active);
    return target.registry_missing_count;
  }
};

TEST(ReviverRecovery, RemoteTargetCannotAuthorizeLocalReplacement) {
  EventDispatcher dispatcher{"reviver_recovery"};
  NetworkInterface network{dispatcher};
  Reviver reviver{dispatcher, network};
  ReviverTestAccess::SetTarget(reviver, Address("192.0.2.1", 30000), 1);
  EXPECT_FALSE(ReviverTestAccess::CanRestart(reviver));
  ReviverTestAccess::Launch(reviver);
  EXPECT_TRUE(ReviverTestAccess::Blocked(reviver));
  EXPECT_EQ(ReviverTestAccess::Status(reviver), "restart_blocked");
  EXPECT_EQ(ReviverTestAccess::LaunchCount(reviver), 0u);
}

TEST(ReviverRecovery, RegistryDisconnectDoesNotCountAsManagerHealthFailure) {
  EventDispatcher dispatcher{"reviver_recovery"};
  NetworkInterface network{dispatcher};
  Reviver reviver{dispatcher, network};
  EXPECT_EQ(ReviverTestAccess::FailedWatcherWithoutRegistry(reviver), 0u);
}

TEST(ReviverRecovery, UnconfirmedRegistryDoesNotAuthorizeAMissingTargetVerdict) {
  EventDispatcher dispatcher{"reviver_recovery"};
  NetworkInterface network{dispatcher};
  Reviver reviver{dispatcher, network};
  EXPECT_EQ(ReviverTestAccess::MissingUnconfirmedRegistry(reviver), 0u);
}

TEST(ReviverRecovery, KnownTargetWithoutProcessIdentityCannotRestart) {
  EventDispatcher dispatcher{"reviver_recovery"};
  NetworkInterface network{dispatcher};
  Reviver reviver{dispatcher, network};
  ReviverTestAccess::SetTarget(reviver, Address("127.0.0.1", 30000), 0);
  EXPECT_FALSE(ReviverTestAccess::CanRestart(reviver));
  ReviverTestAccess::Launch(reviver);
  EXPECT_TRUE(ReviverTestAccess::Blocked(reviver));
  EXPECT_EQ(ReviverTestAccess::LaunchCount(reviver), 0u);
}

TEST(ReviverRecovery, UnseenColdStartDoesNotRequireAnOldProcess) {
  EventDispatcher dispatcher{"reviver_recovery"};
  NetworkInterface network{dispatcher};
  Reviver reviver{dispatcher, network};
  EXPECT_TRUE(ReviverTestAccess::CanRestart(reviver));
}

TEST(ReviverRecovery, LiveLocalProcessCannotAuthorizeReplacement) {
  EventDispatcher dispatcher{"reviver_recovery"};
  NetworkInterface network{dispatcher};
  Reviver reviver{dispatcher, network};
#if defined(_WIN32)
  const auto pid = static_cast<uint32_t>(::_getpid());
#else
  const auto pid = static_cast<uint32_t>(::getpid());
#endif
  ReviverTestAccess::SetTarget(reviver, Address("127.0.0.1", 30000), pid);
  EXPECT_FALSE(ReviverTestAccess::CanRestart(reviver));
  ReviverTestAccess::Launch(reviver);
  EXPECT_TRUE(ReviverTestAccess::Blocked(reviver));
  EXPECT_EQ(ReviverTestAccess::LaunchCount(reviver), 0u);
}

}  // namespace atlas
