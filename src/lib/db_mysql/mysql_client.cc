#include "db_mysql/mysql_client.h"

#include <mysql.h>

namespace atlas {
namespace {

struct MysqlLibrary {
  int status = mysql_library_init(0, nullptr, nullptr);
  ~MysqlLibrary() {
    if (status == 0) mysql_library_end();
  }
};

struct MysqlThread {
  bool initialized = mysql_thread_init() == 0;
  ~MysqlThread() {
    if (initialized) mysql_thread_end();
  }
};

}  // namespace

auto InitializeMysqlThread() -> bool {
  static MysqlLibrary library;
  if (library.status != 0) return false;
  thread_local MysqlThread thread;
  return thread.initialized;
}

}  // namespace atlas
