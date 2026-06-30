#include "server/connection_pool.hpp"

#include <cstdio>
#include <exception>
#include <stdexcept>
#include <utility>

namespace srv {

ConnectionPool::ConnectionPool(std::size_t worker_count, std::size_t queue_capacity,
                               Handler handler)
    : queue_capacity_(queue_capacity), handler_(std::move(handler)) {
  if (worker_count == 0 || queue_capacity_ == 0 || !handler_) {
    throw std::invalid_argument("connection pool requires workers, queue capacity, and a handler");
  }
  workers_.reserve(worker_count);
  try {
    for (std::size_t i = 0; i < worker_count; ++i) {
      workers_.emplace_back([this] { worker_loop(); });
    }
  } catch (...) {
    shutdown();
    throw;
  }
}

ConnectionPool::~ConnectionPool() { shutdown(); }

bool ConnectionPool::try_submit(UniqueFd& client) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_ || queue_.size() >= queue_capacity_) {
      return false;
    }
    queue_.push(std::move(client));
  }
  ready_.notify_one();
  return true;
}

void ConnectionPool::shutdown() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!stopping_) {
      stopping_ = true;
      std::queue<UniqueFd> empty;
      queue_.swap(empty);  // Closing queued sockets is the shutdown policy.
    }
  }
  ready_.notify_all();
  for (std::thread& worker : workers_) {
    if (worker.joinable()) {
      worker.join();
    }
  }
  workers_.clear();
}

void ConnectionPool::worker_loop() {
  for (;;) {
    UniqueFd client;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      ready_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
      if (stopping_) {
        return;
      }
      client = std::move(queue_.front());
      queue_.pop();
    }
    try {
      handler_(std::move(client));
    } catch (const std::exception& error) {
      std::fprintf(stderr, "server: pool connection handler failed: %s\n", error.what());
    } catch (...) {
      std::fprintf(stderr, "server: pool connection handler failed\n");
    }
  }
}

}  // namespace srv
