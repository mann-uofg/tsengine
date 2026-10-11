#include <charconv>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>

#include "tsengine/engine.h"
#include "tsengine/server.h"

namespace {

struct Config {
  std::string dir = "tsengine-data";
  std::string host = "127.0.0.1";
  int port = 8086;
  tsengine::EngineOptions options;
};

constexpr char kUsage[] =
    "usage: tsengine_server [options]\n"
    "  --dir PATH           data directory (default tsengine-data)\n"
    "  --host ADDRESS       address to listen on (default 127.0.0.1)\n"
    "  --port N             port to listen on (default 8086)\n"
    "  --no-sync            do not fsync the WAL on every write; faster, but a power\n"
    "                       failure can lose recent writes\n"
    "  --flush-points N     flush the memtable at this many points\n"
    "  --block-points N     points per block in new files, 1 to 1024\n";

bool ParseNumber(std::string_view text, int64_t* value) {
  const char* begin = text.data();
  const char* end = begin + text.size();
  const auto [ptr, ec] = std::from_chars(begin, end, *value);
  return ec == std::errc() && ptr == end && begin != end && *value >= 0;
}

bool ParseArgs(int argc, char** argv, Config* config) {
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--no-sync") {
      config->options.sync_writes = false;
      continue;
    }
    // Every other option takes a value.
    if (i + 1 >= argc) {
      return false;
    }
    const std::string_view value = argv[++i];
    int64_t number = 0;
    if (arg == "--dir") {
      config->dir = value;
    } else if (arg == "--host") {
      config->host = value;
    } else if (!ParseNumber(value, &number)) {
      return false;
    } else if (arg == "--port") {
      config->port = static_cast<int>(number);
    } else if (arg == "--flush-points") {
      config->options.memtable_flush_points = static_cast<size_t>(number);
    } else if (arg == "--block-points") {
      config->options.block_points = static_cast<size_t>(number);
    } else {
      return false;
    }
  }
  return true;
}

tsengine::Server* g_server = nullptr;

void HandleSignal(int) {
  if (g_server != nullptr) {
    g_server->Stop();
  }
}

}  // namespace

int main(int argc, char** argv) {
  Config config;
  if (!ParseArgs(argc, argv, &config)) {
    std::fputs(kUsage, stderr);
    return 2;
  }

  std::unique_ptr<tsengine::Engine> engine;
  const tsengine::Status s = tsengine::Engine::Open(config.dir, config.options, &engine);
  if (!s.ok()) {
    std::fprintf(stderr, "cannot open %s: %s\n", config.dir.c_str(), s.ToString().c_str());
    return 1;
  }

  tsengine::Server server(engine.get());
  const int port = server.Bind(config.host, config.port);
  if (port < 0) {
    std::fprintf(stderr, "cannot listen on %s:%d\n", config.host.c_str(), config.port);
    return 1;
  }
  g_server = &server;
  std::signal(SIGINT, HandleSignal);
  std::signal(SIGTERM, HandleSignal);
  std::printf("tsengine: data in %s\n", config.dir.c_str());
  std::printf("tsengine: listening on %s:%d\n", config.host.c_str(), port);
  std::fflush(stdout);
  server.Serve();
  return 0;
}
