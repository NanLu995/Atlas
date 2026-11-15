#ifndef ATLAS_TESTS_UNIT_TEST_SHUTDOWN_ACCESS_H_
#define ATLAS_TESTS_UNIT_TEST_SHUTDOWN_ACCESS_H_

#include "server/server_app.h"

namespace atlas {

struct ServerAppShutdownTestAccess {
  static void Expire(ServerApp& app) { app.shutdown_deadline_ = Clock::now(); }
  static auto Failed(const ServerApp& app) -> bool {
    return static_cast<bool>(app.shutdown_error_);
  }
};

}  // namespace atlas

#endif  // ATLAS_TESTS_UNIT_TEST_SHUTDOWN_ACCESS_H_
