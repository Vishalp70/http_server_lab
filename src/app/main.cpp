#include <iostream>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <string>
#include <system_error>
#include <vector>

#include "app/cli.hpp"
#include "common/version.hpp"
#include "net/tcp_server.hpp"

namespace {

constexpr int kExitOk = 0;
constexpr int kExitUsage = 2;
constexpr int kExitRuntime = 1;
static_assert(std::atomic_bool::is_always_lock_free,
              "the signal handler requires lock-free atomic bool operations");
std::atomic_bool stop_requested{false};

void request_stop(int) { stop_requested.store(true, std::memory_order_relaxed); }

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> args;
  for (int i = 1; i < argc; ++i) {
    args.emplace_back(argv[i]);
  }

  const srv::CliResult cli = srv::parse_cli(args);

  switch (cli.action) {
    case srv::CliAction::kShowHelp:
      std::cout << srv::usage_text();
      return kExitOk;

    case srv::CliAction::kShowVersion:
      std::cout << srv::version_text() << '\n';
      return kExitOk;

    case srv::CliAction::kError:
      std::cerr << srv::kProgramName << ": error: " << cli.error << "\n"
                << "Try '" << srv::kProgramName << " --help' for usage.\n";
      return kExitUsage;

    case srv::CliAction::kRun:
      break;
  }

  struct sigaction action {};
  action.sa_handler = request_stop;
  ::sigemptyset(&action.sa_mask);
  action.sa_flags = 0;  // Let signals interrupt poll/recv so shutdown is prompt.
  if (::sigaction(SIGINT, &action, nullptr) == -1 ||
      ::sigaction(SIGTERM, &action, nullptr) == -1) {
    std::cerr << srv::kProgramName << ": unable to install signal handlers: "
              << std::strerror(errno) << '\n';
    return kExitRuntime;
  }

  int result = -1;
  switch (cli.config.mode) {
    case srv::ServerMode::kThreadPerConnection:
    case srv::ServerMode::kThreadPool:
      result = srv::run_http_server(cli.config, &stop_requested);
      break;
    case srv::ServerMode::kEpoll:
      result = srv::run_epoll_server(cli.config, &stop_requested);
      break;
  }
  return result == 0 ? kExitOk : kExitRuntime;
}
