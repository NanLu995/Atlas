#ifndef ATLAS_TESTS_INTEGRATION_MYSQL_TEST_FIXTURE_H_
#define ATLAS_TESTS_INTEGRATION_MYSQL_TEST_FIXTURE_H_

#include <mysql.h>

#ifdef SendMessage
#undef SendMessage
#endif

#include <charconv>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "db_mysql/mysql_client.h"
#include "platform/environment.h"

namespace atlas::test_fixtures {

inline auto MysqlTestEnvironment(const char* name) -> std::string {
  auto value = ReadEnvironmentVariable(name);
  return value ? *value : "";
}

inline auto MysqlTestArguments() -> std::vector<std::string> {
  return {"--db-type",
          "mysql",
          "--db-mysql-host",
          MysqlTestEnvironment("ATLAS_TEST_MYSQL_HOST"),
          "--db-mysql-port",
          MysqlTestEnvironment("ATLAS_TEST_MYSQL_PORT"),
          "--db-mysql-user",
          MysqlTestEnvironment("ATLAS_TEST_MYSQL_USER"),
          "--db-mysql-database",
          MysqlTestEnvironment("ATLAS_TEST_MYSQL_DATABASE"),
          "--db-mysql-database-id",
          "0123456789abcdef0123456789abcdef",
          "--db-mysql-ssl-ca",
          MysqlTestEnvironment("ATLAS_TEST_MYSQL_SSL_CA"),
          "--db-mysql-initialize",
          "true"};
}

inline auto MysqlTestConnection() -> std::unique_ptr<MYSQL, decltype(&mysql_close)> {
  std::unique_ptr<MYSQL, decltype(&mysql_close)> connection(nullptr, mysql_close);
  if (MysqlTestEnvironment("ATLAS_TEST_MYSQL") != "1") {
    ADD_FAILURE() << "MySQL environment blocked: run through tools/bin/test_mysql";
    return connection;
  }
  for (const char* key : {"HOST", "PORT", "USER", "PASSWORD", "DATABASE", "SSL_CA"}) {
    if (MysqlTestEnvironment((std::string("ATLAS_TEST_MYSQL_") + key).c_str()).empty()) {
      ADD_FAILURE() << "MySQL environment blocked: missing ATLAS_TEST_MYSQL_" << key;
      return connection;
    }
  }
  unsigned int port = 0;
  const auto text = MysqlTestEnvironment("ATLAS_TEST_MYSQL_PORT");
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), port);
  if (error != std::errc{} || end != text.data() + text.size() || port == 0 || port > 65535) {
    ADD_FAILURE() << "MySQL environment blocked: invalid ATLAS_TEST_MYSQL_PORT";
    return connection;
  }
  if (!InitializeMysqlThread()) return connection;
  connection.reset(mysql_init(nullptr));
  if (!connection) return connection;
  unsigned int protocol = MYSQL_PROTOCOL_TCP;
  unsigned int timeout = 5;
  enum mysql_ssl_mode mode = SSL_MODE_VERIFY_IDENTITY;
  const auto ca = MysqlTestEnvironment("ATLAS_TEST_MYSQL_SSL_CA");
  if (mysql_options(connection.get(), MYSQL_OPT_PROTOCOL, &protocol) != 0 ||
      mysql_options(connection.get(), MYSQL_OPT_CONNECT_TIMEOUT, &timeout) != 0 ||
      mysql_options(connection.get(), MYSQL_OPT_SSL_CA, ca.c_str()) != 0 ||
      mysql_options(connection.get(), MYSQL_OPT_SSL_MODE, &mode) != 0 ||
      !mysql_real_connect(connection.get(), MysqlTestEnvironment("ATLAS_TEST_MYSQL_HOST").c_str(),
                          MysqlTestEnvironment("ATLAS_TEST_MYSQL_USER").c_str(),
                          MysqlTestEnvironment("ATLAS_TEST_MYSQL_PASSWORD").c_str(),
                          MysqlTestEnvironment("ATLAS_TEST_MYSQL_DATABASE").c_str(), port, nullptr,
                          0)) {
    ADD_FAILURE() << "isolated MySQL connection failed";
    connection.reset();
  }
  return connection;
}

inline auto ResetMysqlTestDatabase() -> bool {
  auto connection = MysqlTestConnection();
  return connection &&
         mysql_query(
             connection.get(),
             "DROP TABLE IF EXISTS entities, meta, atlas_entity_id_counter, atlas_write_receipts, "
             "atlas_checkout_versions, atlas_checkout_processes, atlas_account_sessions") == 0;
}

}  // namespace atlas::test_fixtures

#endif  // ATLAS_TESTS_INTEGRATION_MYSQL_TEST_FIXTURE_H_
