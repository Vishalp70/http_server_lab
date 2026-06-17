#include "net/tcp_server.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <system_error>
#include <stdexcept>
#include <memory>
#include <thread>
#include <vector>

#include "http/http.hpp"
#include "net/unique_fd.hpp"
#include "server/connection_pool.hpp"

namespace srv {
namespace {

constexpr int kListenBacklog = 64;
constexpr std::size_t kMaxWorkerThreads = 128;

struct Worker {
  std::thread thread;
  std::shared_ptr<std::atomic_bool> finished;
};

UniqueFd create_listener(const ServerConfig& config, std::uint16_t* bound_port) {
  UniqueFd listener(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
  if (!listener) {
    throw std::system_error(errno, std::generic_category(), "socket");
  }

  int reuse_address = 1;
  if (::setsockopt(listener.get(), SOL_SOCKET, SO_REUSEADDR, &reuse_address,
                   sizeof(reuse_address)) == -1) {
    throw std::system_error(errno, std::generic_category(), "setsockopt(SO_REUSEADDR)");
  }

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(config.port);
  if (::inet_pton(AF_INET, config.bind_address.c_str(), &address.sin_addr) != 1) {
    throw std::invalid_argument("invalid IPv4 bind address '" + config.bind_address + "'");
  }
  if (::bind(listener.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == -1) {
    throw std::system_error(errno, std::generic_category(), "bind");
  }
  if (::listen(listener.get(), kListenBacklog) == -1) {
    throw std::system_error(errno, std::generic_category(), "listen");
  }

  socklen_t address_size = sizeof(address);
  if (::getsockname(listener.get(), reinterpret_cast<sockaddr*>(&address), &address_size) == -1) {
    throw std::system_error(errno, std::generic_category(), "getsockname");
  }
  *bound_port = ntohs(address.sin_port);
  return listener;
}

bool send_response(int client_fd, const std::string& response,
                   const std::atomic_bool* stop_requested) {
  std::size_t sent = 0;
  while (sent < response.size() && !stop_requested->load(std::memory_order_relaxed)) {
    const ssize_t count = ::send(client_fd, response.data() + sent, response.size() - sent,
                                 MSG_NOSIGNAL);
    if (count > 0) {
      sent += static_cast<std::size_t>(count);
      continue;
    }
    if (count == 0) {
      return true;
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EPIPE || errno == ECONNRESET) {
      return true;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      pollfd writable{client_fd, POLLOUT, 0};
      const int ready = ::poll(&writable, 1, 250);
      if (ready < 0 && errno != EINTR && errno != EAGAIN) {
        std::perror("poll(client write)");
        return false;
      }
      continue;
    }
    std::perror("send");
    return false;
  }
  return true;
}

void reject_overloaded_client(int client_fd, const std::atomic_bool* stop_requested) {
  (void)send_response(client_fd, make_service_unavailable_response(), stop_requested);
  (void)::shutdown(client_fd, SHUT_WR);

  std::array<char, 1024> discarded{};
  for (int attempt = 0; attempt < 4; ++attempt) {
    pollfd readable{client_fd, POLLIN, 0};
    const int ready = ::poll(&readable, 1, 50);
    if (ready <= 0) {
      if (ready < 0 && errno == EINTR) {
        continue;
      }
      break;
    }
    for (;;) {
      const ssize_t count = ::recv(client_fd, discarded.data(), discarded.size(), 0);
      if (count > 0) {
        continue;
      }
      if (count == 0) {
        return;
      }
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        break;
      }
      return;
    }
  }
}

bool serve_http_client(int client_fd, const std::atomic_bool* stop_requested) {
  std::array<char, 1024> buffer{};
  std::string request;
  for (;;) {
    if (stop_requested->load(std::memory_order_relaxed)) {
      return true;
    }
    pollfd readiness{client_fd, POLLIN, 0};
    const int ready = ::poll(&readiness, 1, 250);
    if (ready == 0) {
      continue;
    }
    if (ready < 0) {
      if (errno == EINTR || errno == EAGAIN) {
        continue;
      }
      std::perror("poll(client read)");
      return false;
    }

    const ssize_t received = ::recv(client_fd, buffer.data(), buffer.size(), 0);
    if (received == 0) {
      if (request.empty()) {
        return true;
      }
      return send_response(client_fd, handle_http_request(request), stop_requested);
    }
    if (received < 0) {
      if (errno == EINTR) {
        if (stop_requested->load(std::memory_order_relaxed)) {
          return true;
        }
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        continue;
      }
      if (errno == ECONNRESET) {
        return true;
      }
      std::perror("recv");
      return false;
    }

    const std::size_t received_size = static_cast<std::size_t>(received);
    if (request.size() + received_size > kMaxHttpRequestBytes) {
      const std::size_t bytes_to_limit = kMaxHttpRequestBytes + 1 - request.size();
      request.append(buffer.data(), bytes_to_limit);
      return send_response(client_fd, handle_http_request(request), stop_requested);
    }
    request.append(buffer.data(), received_size);
    if (request.find("\r\n\r\n") != std::string::npos) {
      return send_response(client_fd, handle_http_request(request), stop_requested);
    }
  }
}

}  // namespace

int run_http_server(const ServerConfig& config, const std::atomic_bool* stop_requested) {
  try {
    std::unique_ptr<ConnectionPool> pool;
    if (config.mode == ServerMode::kThreadPool) {
      pool = std::make_unique<ConnectionPool>(
          config.worker_count, kMaxPendingConnections,
          [stop_requested](UniqueFd client) {
            (void)serve_http_client(client.get(), stop_requested);
          });
    }

    std::vector<Worker> workers;
    workers.reserve(kMaxWorkerThreads);
    const auto reap_workers = [&workers]() {
      for (auto worker = workers.begin(); worker != workers.end();) {
        if (worker->finished->load(std::memory_order_acquire)) {
          worker->thread.join();
          worker = workers.erase(worker);
        } else {
          ++worker;
        }
      }
    };

    std::uint16_t bound_port = 0;
    UniqueFd listener = create_listener(config, &bound_port);
    std::printf("READY %s:%u\n", config.bind_address.c_str(),
                static_cast<unsigned int>(bound_port));
    std::fflush(stdout);

    while (!stop_requested->load(std::memory_order_relaxed)) {
      if (!pool) {
        reap_workers();
      }
      pollfd readiness{listener.get(), POLLIN, 0};
      const int ready = ::poll(&readiness, 1, 250);
      if (ready == 0) {
        continue;
      }
      if (ready < 0) {
        if (errno == EINTR && stop_requested->load(std::memory_order_relaxed)) {
          break;
        }
        if (errno == EINTR) {
          continue;
        }
        throw std::system_error(errno, std::generic_category(), "poll");
      }
      if ((readiness.revents & (POLLERR | POLLNVAL)) != 0) {
        throw std::system_error(EIO, std::generic_category(), "listener poll error");
      }
      if ((readiness.revents & POLLIN) == 0) {
        continue;
      }

      sockaddr_in peer{};
      socklen_t peer_size = sizeof(peer);
      UniqueFd client(::accept4(listener.get(), reinterpret_cast<sockaddr*>(&peer), &peer_size,
                                SOCK_CLOEXEC | SOCK_NONBLOCK));
      if (!client) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          continue;
        }
        if (errno == EINTR && stop_requested->load(std::memory_order_relaxed)) {
          break;
        }
        if (errno == EINTR || errno == ECONNABORTED) {
          continue;
        }
        std::perror("accept4");
        return -1;
      }
      if (pool) {
        if (!pool->try_submit(client)) {
          reject_overloaded_client(client.get(), stop_requested);
        }
        continue;
      }
      if (workers.size() >= kMaxWorkerThreads) {
        reject_overloaded_client(client.get(), stop_requested);
        continue;
      }

      auto finished = std::make_shared<std::atomic_bool>(false);
      try {
        std::thread worker([client = std::move(client), stop_requested, finished]() mutable {
          try {
            (void)serve_http_client(client.get(), stop_requested);
          } catch (const std::exception& error) {
            std::fprintf(stderr, "server: connection handler failed: %s\n", error.what());
          } catch (...) {
            std::fprintf(stderr, "server: connection handler failed\n");
          }
          finished->store(true, std::memory_order_release);
        });
        workers.push_back({std::move(worker), std::move(finished)});
      } catch (const std::system_error& error) {
        std::fprintf(stderr, "server: could not start connection thread: %s\n", error.what());
      }
    }

    if (pool) {
      pool->shutdown();
    } else {
      for (Worker& worker : workers) {
        if (worker.thread.joinable()) {
          worker.thread.join();
        }
      }
    }
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "server: %s\n", error.what());
    return -1;
  }
}

}  // namespace srv
