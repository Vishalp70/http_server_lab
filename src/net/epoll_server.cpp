#include "net/tcp_server.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>

#include "http/http.hpp"
#include "net/unique_fd.hpp"

namespace srv {
namespace {

constexpr int kListenBacklog = 128;
constexpr std::size_t kMaxConnections = 1024;
constexpr int kMaxAcceptsPerEvent = 64;
constexpr std::uint64_t kListenerToken = 1;

struct Connection {
  explicit Connection(UniqueFd descriptor) : fd(std::move(descriptor)) {}

  UniqueFd fd;
  std::string input;
  std::string output;
  std::size_t output_offset = 0;
};

UniqueFd make_listener(const ServerConfig& config, std::uint16_t* bound_port) {
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

}  // namespace

int run_epoll_server(const ServerConfig& config, const std::atomic_bool* stop_requested) {
  try {
    std::uint16_t bound_port = 0;
    UniqueFd listener = make_listener(config, &bound_port);
    UniqueFd epoll_fd(::epoll_create1(EPOLL_CLOEXEC));
    if (!epoll_fd) {
      throw std::system_error(errno, std::generic_category(), "epoll_create1");
    }

    epoll_event listen_event{};
    listen_event.events = EPOLLIN;
    listen_event.data.u64 = kListenerToken;
    if (::epoll_ctl(epoll_fd.get(), EPOLL_CTL_ADD, listener.get(), &listen_event) == -1) {
      throw std::system_error(errno, std::generic_category(), "epoll_ctl(ADD listener)");
    }

    std::printf("READY %s:%u\n", config.bind_address.c_str(),
                static_cast<unsigned int>(bound_port));
    std::fflush(stdout);

    std::unordered_map<std::uint64_t, Connection> connections;
    connections.reserve(kMaxConnections);
    std::uint64_t next_token = kListenerToken + 1;
    std::array<epoll_event, 128> ready_events{};

    while (!stop_requested->load(std::memory_order_relaxed)) {
      const int count = ::epoll_wait(epoll_fd.get(), ready_events.data(),
                                     static_cast<int>(ready_events.size()), 250);
      if (count < 0) {
        if (errno == EINTR && stop_requested->load(std::memory_order_relaxed)) {
          break;
        }
        if (errno == EINTR) {
          continue;
        }
        throw std::system_error(errno, std::generic_category(), "epoll_wait");
      }

      for (int index = 0; index < count; ++index) {
        const epoll_event event = ready_events[static_cast<std::size_t>(index)];
        const std::uint64_t token = event.data.u64;
        if (token == kListenerToken) {
          for (int accepted = 0; accepted < kMaxAcceptsPerEvent; ++accepted) {
            sockaddr_in peer{};
            socklen_t peer_size = sizeof(peer);
            UniqueFd client(::accept4(listener.get(), reinterpret_cast<sockaddr*>(&peer), &peer_size,
                                      SOCK_CLOEXEC | SOCK_NONBLOCK));
            if (!client) {
              if (errno == EINTR || errno == ECONNABORTED) {
                --accepted;
                continue;
              }
              if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
              }
              std::perror("accept4");
              break;
            }
            if (connections.size() >= kMaxConnections || next_token == 0) {
              continue;  // The bounded epoll policy closes excess connections.
            }

            const std::uint64_t client_token = next_token++;
            auto [connection, inserted] =
                connections.emplace(client_token, Connection(std::move(client)));
            if (!inserted) {
              continue;
            }
            epoll_event client_event{};
            client_event.events = EPOLLIN | EPOLLRDHUP;
            client_event.data.u64 = client_token;
            if (::epoll_ctl(epoll_fd.get(), EPOLL_CTL_ADD, connection->second.fd.get(),
                            &client_event) == -1) {
              std::perror("epoll_ctl(ADD client)");
              connections.erase(connection);
            }
          }
          continue;
        }

        auto connection = connections.find(token);
        if (connection == connections.end()) {
          continue;  // A stale event for a connection already closed this batch.
        }
        const auto close_connection = [&]() {
          (void)::epoll_ctl(epoll_fd.get(), EPOLL_CTL_DEL, connection->second.fd.get(), nullptr);
          connections.erase(connection);
        };
        if ((event.events & EPOLLERR) != 0) {
          close_connection();
          continue;
        }

        Connection& state = connection->second;
        bool read_failed = false;
        if (state.output.empty() &&
            (event.events & (EPOLLIN | EPOLLRDHUP | EPOLLHUP)) != 0) {
          std::array<char, 4096> input_buffer{};
          for (;;) {
            const ssize_t received =
                ::recv(state.fd.get(), input_buffer.data(), input_buffer.size(), 0);
            if (received > 0) {
              const std::size_t received_size = static_cast<std::size_t>(received);
              if (state.input.size() + received_size > kMaxHttpRequestBytes) {
                const std::size_t through_limit = kMaxHttpRequestBytes + 1 - state.input.size();
                state.input.append(input_buffer.data(), through_limit);
                state.output = handle_http_request(state.input);
                break;
              }
              state.input.append(input_buffer.data(), received_size);
              if (state.input.find("\r\n\r\n") != std::string::npos) {
                state.output = handle_http_request(state.input);
                break;
              }
              continue;
            }
            if (received == 0) {
              if (state.input.empty()) {
                read_failed = true;
              } else {
                state.output = handle_http_request(state.input);
              }
              break;
            }
            if (errno == EINTR) {
              continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
              break;
            }
            read_failed = true;
            break;
          }
        }
        if (read_failed || ((event.events & EPOLLHUP) != 0 && state.output.empty())) {
          close_connection();
          continue;
        }

        if (!state.output.empty() && state.output_offset == 0) {
          epoll_event response_event{};
          response_event.events = EPOLLOUT | EPOLLRDHUP;
          response_event.data.u64 = token;
          if (::epoll_ctl(epoll_fd.get(), EPOLL_CTL_MOD, state.fd.get(), &response_event) == -1) {
            close_connection();
            continue;
          }
        }

        if ((event.events & EPOLLOUT) != 0 && !state.output.empty()) {
          bool write_failed = false;
          while (state.output_offset < state.output.size()) {
            const ssize_t sent = ::send(state.fd.get(), state.output.data() + state.output_offset,
                                        state.output.size() - state.output_offset, MSG_NOSIGNAL);
            if (sent > 0) {
              state.output_offset += static_cast<std::size_t>(sent);
              continue;
            }
            if (sent < 0 && errno == EINTR) {
              continue;
            }
            if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
              break;
            }
            write_failed = true;
            break;
          }
          if (write_failed || state.output_offset == state.output.size()) {
            close_connection();
          }
        }
      }
    }
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "epoll server: %s\n", error.what());
    return -1;
  }
}

}  // namespace srv
