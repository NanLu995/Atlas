#include <cstdio>
#include <string>
#include <vector>

#include "dbapp/dbapp.h"
#include "platform/environment.h"

namespace {

class DatabaseProcess final : public atlas::DBApp {
 public:
  using DBApp::DBApp;

 protected:
  auto Init(int argc, char* argv[]) -> bool override {
    if (!DBApp::Init(argc, argv)) return false;
    std::puts("DBAPP_TEST_READY");
    std::fflush(stdout);
    return true;
  }
};

}  // namespace

int main(int argc, char* argv[]) {
  std::vector<std::string> values;
  for (int i = 0; i < argc; ++i) values.emplace_back(argv[i]);
  for (const auto* field : {"HOST", "PORT", "USER", "PASSWORD", "DATABASE", "SSL_CA"}) {
    auto value = atlas::ReadEnvironmentVariable((std::string("ATLAS_TEST_MYSQL_") + field).c_str());
    if (!value || value->empty()) return 2;
    std::string option = field;
    for (auto& character : option) {
      if (character == '_') character = '-';
      if (character >= 'A' && character <= 'Z') character = static_cast<char>(character + 32);
    }
    values.emplace_back("--db-mysql-" + option);
    values.push_back(std::move(*value));
  }
  std::vector<char*> args;
  for (auto& value : values) args.push_back(value.data());
  atlas::EventDispatcher dispatcher;
  atlas::NetworkInterface network(dispatcher);
  DatabaseProcess process(dispatcher, network);
  return process.RunApp(static_cast<int>(args.size()), args.data());
}
