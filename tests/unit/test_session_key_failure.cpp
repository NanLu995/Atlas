#include <gtest/gtest.h>

#include "server/entity_types.h"
#include "server/server_app.h"

namespace atlas {

auto FillSecureRandom(std::span<std::byte>) -> Result<void> {
  return Error{ErrorCode::kIoError, "injected entropy failure"};
}

TEST(SessionKey, EntropyFailureDoesNotProduceAKey) {
  auto key = SessionKey::Generate();
  ASSERT_FALSE(key.HasValue());
  EXPECT_EQ(key.Error().Code(), ErrorCode::kIoError);
}

TEST(ServerProcessIdentity, EntropyFailureRejectsStartup) {
  class TestApp : public ServerApp {
   public:
    using ServerApp::Init;
    using ServerApp::ServerApp;
  };
  EventDispatcher dispatcher("entropy_failure");
  NetworkInterface network(dispatcher);
  TestApp app(dispatcher, network);
  EXPECT_FALSE(app.Init(0, nullptr));
  EXPECT_EQ(app.Config().process_instance, 0u);
}

}  // namespace atlas
