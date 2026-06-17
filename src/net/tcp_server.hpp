#pragma once

#include <atomic>

#include "common/config.hpp"

namespace srv {

// Runs the bounded thread-per-connection HTTP server until stop is requested.
// Returns 0 on graceful shutdown and -1 after a reported system error.
int run_http_server(const ServerConfig& config,
                    const std::atomic_bool* stop_requested);
int run_epoll_server(const ServerConfig& config,
                     const std::atomic_bool* stop_requested);

}  // namespace srv
