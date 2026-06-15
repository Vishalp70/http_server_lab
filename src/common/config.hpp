#pragma once

#include <cstdint>

namespace srv {

// Runtime configuration of the server. Milestone 1 only carries the listening
// port; later milestones extend this struct (bind address, timeouts, limits).
struct ServerConfig {
  static constexpr std::uint16_t kDefaultPort = 8080;

  // 0 means "let the kernel pick an ephemeral port" (useful for tests).
  std::uint16_t port = kDefaultPort;
};

}  // namespace srv
