#pragma once

#include <cstdint>
#include <string>

namespace srv {

enum class ServerMode { kThreadPerConnection, kThreadPool, kEpoll };

// Runtime configuration of the server.
struct ServerConfig {
  static constexpr std::uint16_t kDefaultPort = 8080;
  static constexpr const char* kDefaultBindAddress = "127.0.0.1";

  // 0 means "let the kernel pick an ephemeral port" (useful for tests).
  std::uint16_t port = kDefaultPort;
  std::string bind_address = kDefaultBindAddress;
  ServerMode mode = ServerMode::kThreadPerConnection;
  std::uint16_t worker_count = 4;
};

}  // namespace srv
