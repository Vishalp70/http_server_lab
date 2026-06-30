#pragma once

#include <cstddef>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <thread>
#include <vector>

#include "net/unique_fd.hpp"

namespace srv {

inline constexpr std::size_t kMaxPendingConnections = 16;

// A bounded pool whose tasks each own and serve one client connection.
class ConnectionPool {
 public:
  using Handler = std::function<void(UniqueFd)>;

  ConnectionPool(std::size_t worker_count, std::size_t queue_capacity, Handler handler);
  ~ConnectionPool();

  ConnectionPool(const ConnectionPool&) = delete;
  ConnectionPool& operator=(const ConnectionPool&) = delete;

  // Moves the descriptor into the queue only when a queue slot is available.
  bool try_submit(UniqueFd& client);
  void shutdown();

 private:
  void worker_loop();

  const std::size_t queue_capacity_;
  Handler handler_;
  std::mutex mutex_;
  std::condition_variable ready_;
  std::queue<UniqueFd> queue_;
  std::vector<std::thread> workers_;
  bool stopping_ = false;
};

}  // namespace srv
