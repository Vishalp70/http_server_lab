#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <atomic>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <chrono>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "net/tcp_server.hpp"
#include "http/http.hpp"
#include "server/connection_pool.hpp"

#ifndef HTTPD_PATH
#error "HTTPD_PATH must point to the built server executable"
#endif

namespace {

int open_connection(int port);

class ChildServer {
 public:
  explicit ChildServer(std::string mode = "thread-per-connection", std::string workers = "4")
      : mode_(std::move(mode)), workers_(std::move(workers)) {
    int pipe_fds[2];
    if (::pipe(pipe_fds) != 0) {
      ADD_FAILURE() << "pipe failed";
      return;
    }
    pid_ = ::fork();
    if (pid_ == 0) {
      ::close(pipe_fds[0]);
      if (::dup2(pipe_fds[1], STDOUT_FILENO) == -1) {
        _exit(126);
      }
      ::close(pipe_fds[1]);
      if (mode_ == "thread-pool") {
        ::execl(HTTPD_PATH, HTTPD_PATH, "--mode", mode_.c_str(), "--workers", workers_.c_str(),
                "--bind", "127.0.0.1", "--port", "0", static_cast<char*>(nullptr));
      } else {
        ::execl(HTTPD_PATH, HTTPD_PATH, "--mode", mode_.c_str(), "--bind", "127.0.0.1",
                "--port", "0", static_cast<char*>(nullptr));
      }
      _exit(127);
    }
    ::close(pipe_fds[1]);
    output_fd_ = pipe_fds[0];
    if (pid_ < 0) {
      ADD_FAILURE() << "fork failed";
      ::close(output_fd_);
      output_fd_ = -1;
    }
  }

  ~ChildServer() {
    if (pid_ > 0) {
      stop_and_wait();
    }
    if (output_fd_ >= 0) {
      ::close(output_fd_);
    }
  }

  ChildServer(const ChildServer&) = delete;
  ChildServer& operator=(const ChildServer&) = delete;

  pid_t pid() const { return pid_; }

  int stop_and_wait() {
    if (pid_ <= 0) {
      return -1;
    }
    (void)::kill(pid_, SIGTERM);
    int status = 0;
    pid_t waited = -1;
    do {
      waited = ::waitpid(pid_, &status, 0);
    } while (waited == -1 && errno == EINTR);
    if (waited == pid_) {
      pid_ = -1;
      return status;
    }
    return -1;
  }

  int wait_for_port() {
    if (output_fd_ < 0) {
      return -1;
    }
    pollfd descriptor{output_fd_, POLLIN, 0};
    std::string line;
    for (;;) {
      const int ready = ::poll(&descriptor, 1, 5000);
      if (ready <= 0) {
        ADD_FAILURE() << "server did not announce readiness within five seconds";
        return -1;
      }
      char c = '\0';
      const ssize_t count = ::read(output_fd_, &c, 1);
      if (count != 1) {
        ADD_FAILURE() << "server exited before announcing readiness";
        return -1;
      }
      if (c == '\n') {
        break;
      }
      line.push_back(c);
    }
    constexpr char kPrefix[] = "READY 127.0.0.1:";
    if (line.rfind(kPrefix, 0) != 0) {
      ADD_FAILURE() << "unexpected server startup line: " << line;
      return -1;
    }
    try {
      return std::stoi(line.substr(sizeof(kPrefix) - 1));
    } catch (...) {
      ADD_FAILURE() << "invalid port in server startup line: " << line;
      return -1;
    }
  }

 private:
  pid_t pid_ = -1;
  int output_fd_ = -1;
  std::string mode_;
  std::string workers_;
};

class OpenClients {
 public:
  ~OpenClients() {
    for (const int fd : fds_) {
      ::close(fd);
    }
  }
  int open(int port) {
    const int fd = open_connection(port);
    if (fd >= 0) {
      fds_.push_back(fd);
    }
    return fd;
  }

 private:
  std::vector<int> fds_;
};

bool send_all(int fd, const std::vector<char>& data) {
  std::size_t offset = 0;
  while (offset < data.size()) {
    const ssize_t count = ::send(fd, data.data() + offset, data.size() - offset, MSG_NOSIGNAL);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      return false;
    }
    offset += static_cast<std::size_t>(count);
  }
  return true;
}

int open_connection(int port) {
  const int client = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (client < 0) {
    return -1;
  }
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(static_cast<std::uint16_t>(port));
  (void)::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
  if (::connect(client, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
    ::close(client);
    return -1;
  }
  return client;
}

std::string http_exchange(int port, const std::string& request, std::size_t split_at = 0) {
  const int client = open_connection(port);
  if (client < 0) {
    return "<connect failed>";
  }
  const std::size_t split = split_at > 0 && split_at < request.size() ? split_at : request.size();
  bool sent = send_all(client, std::vector<char>(request.begin(), request.begin() + split));
  if (sent && split < request.size()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    sent = send_all(client, std::vector<char>(request.begin() + split, request.end()));
  }
  if (!sent || ::shutdown(client, SHUT_WR) != 0) {
    if (client >= 0) {
      ::close(client);
    }
    return "<send failed>";
  }

  std::string actual;
  char buffer[8192];
  for (;;) {
    pollfd readiness{client, POLLIN, 0};
    if (::poll(&readiness, 1, 3000) <= 0) {
      ::close(client);
      return "<read timeout>";
    }
    const ssize_t count = ::recv(client, buffer, sizeof(buffer), 0);
    if (count == 0) {
      break;
    }
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count < 0) {
      ::close(client);
      return "<recv failed>";
    }
    actual.append(buffer, static_cast<std::size_t>(count));
  }
  ::close(client);
  return actual.empty() ? "<empty response>" : actual;
}

TEST(HttpServer, ServesEndpointsAndErrorsForConcurrentAndSequentialClientsAndShutsDown) {
  ChildServer server;
  ASSERT_GT(server.pid(), 0);
  const int port = server.wait_for_port();
  ASSERT_GT(port, 0);

  const int slow_client = open_connection(port);
  ASSERT_GE(slow_client, 0);
  const std::string parallel_health =
      http_exchange(port, "GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n");
  ::close(slow_client);
  EXPECT_NE(parallel_health.find("HTTP/1.1 200 OK\r\n"), std::string::npos);

  const std::string root = http_exchange(port, "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n");
  const std::string health =
      http_exchange(port, "GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n");
  const std::string hello =
      http_exchange(port, "GET /hello HTTP/1.1\r\nHost: localhost\r\n\r\n");
  const std::string missing =
      http_exchange(port, "GET /missing HTTP/1.1\r\nHost: localhost\r\n\r\n");
  const std::string method =
      http_exchange(port, "POST / HTTP/1.1\r\nHost: localhost\r\n\r\n");
  const std::string malformed = http_exchange(port, "not a request\r\n\r\n");
  const std::string oversized = http_exchange(
      port, std::string(srv::kMaxHttpRequestBytes + 1, 'x'));

  EXPECT_NE(root.find("HTTP/1.1 200 OK\r\n"), std::string::npos);
  EXPECT_NE(root.find("Linux HTTP server lab\n"), std::string::npos);
  EXPECT_NE(health.find("\r\n\r\nOK\n"), std::string::npos);
  EXPECT_NE(hello.find("\r\n\r\nHello!\n"), std::string::npos);
  EXPECT_NE(missing.find("HTTP/1.1 404 Not Found\r\n"), std::string::npos);
  EXPECT_NE(method.find("HTTP/1.1 405 Method Not Allowed\r\n"), std::string::npos);
  EXPECT_NE(malformed.find("HTTP/1.1 400 Bad Request\r\n"), std::string::npos);
  EXPECT_NE(oversized.find("HTTP/1.1 431 Request Header Fields Too Large\r\n"),
            std::string::npos);
  EXPECT_NE(root.find("Connection: close\r\n"), std::string::npos);

  const int status = server.stop_and_wait();
  ASSERT_NE(status, -1);
  EXPECT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 0);
}

TEST(HttpServer, RejectsInvalidIpv4BindAddress) {
  srv::ServerConfig config;
  config.bind_address = "localhost";
  std::atomic_bool stop_requested{false};
  EXPECT_EQ(srv::run_http_server(config, &stop_requested), -1);
}

TEST(HttpServer, ThreadPoolReturns503WhenItsQueueIsFull) {
  ChildServer server("thread-pool", "1");
  ASSERT_GT(server.pid(), 0);
  const int port = server.wait_for_port();
  ASSERT_GT(port, 0);

  OpenClients clients;
  for (std::size_t i = 0; i < srv::kMaxPendingConnections + 1; ++i) {
    ASSERT_GE(clients.open(port), 0);
  }
  const std::string overloaded =
      http_exchange(port, "GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n");
  EXPECT_NE(overloaded.find("HTTP/1.1 503 Service Unavailable\r\n"), std::string::npos)
      << "response was: " << overloaded;

  const int status = server.stop_and_wait();
  ASSERT_NE(status, -1);
  EXPECT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 0);
}

TEST(HttpServer, EpollHandlesPartialRequestsDisconnectsAndRepeatedClients) {
  ChildServer server("epoll");
  ASSERT_GT(server.pid(), 0);
  const int port = server.wait_for_port();
  ASSERT_GT(port, 0);

  const int slow_client = open_connection(port);
  ASSERT_GE(slow_client, 0);
  const std::string concurrent =
      http_exchange(port, "GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n");
  ::close(slow_client);
  EXPECT_NE(concurrent.find("HTTP/1.1 200 OK\r\n"), std::string::npos);

  const std::string partial = http_exchange(
      port, "GET /hello HTTP/1.1\r\nHost: localhost\r\n\r\n", 14);
  EXPECT_NE(partial.find("\r\n\r\nHello!\n"), std::string::npos);

  for (int i = 0; i < 40; ++i) {
    const int disconnected = open_connection(port);
    ASSERT_GE(disconnected, 0);
    ::close(disconnected);
    const std::string response =
        http_exchange(port, "GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n");
    EXPECT_NE(response.find("HTTP/1.1 200 OK\r\n"), std::string::npos);
  }

  const int status = server.stop_and_wait();
  ASSERT_NE(status, -1);
  EXPECT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 0);
}

}  // namespace
