#include "app/cli.hpp"

#include <charconv>
#include <cstddef>
#include <system_error>
#include <utility>

#include "common/version.hpp"

namespace srv {
namespace {

constexpr std::string_view kPortEqualsPrefix = "--port=";
constexpr std::string_view kBindEqualsPrefix = "--bind=";
constexpr std::string_view kModeEqualsPrefix = "--mode=";
constexpr std::string_view kWorkersEqualsPrefix = "--workers=";

CliResult make_error(std::string message) {
  CliResult result;
  result.action = CliAction::kError;
  result.error = std::move(message);
  return result;
}

CliResult make_action(CliAction action) {
  CliResult result;
  result.action = action;
  return result;
}

}  // namespace

std::optional<std::uint16_t> parse_port(std::string_view text) {
  if (text.empty()) {
    return std::nullopt;
  }
  // Explicit digit check: std::from_chars alone would accept a leading '-'
  // for some integer types, and we want a single, obvious rule.
  for (const char c : text) {
    if (c < '0' || c > '9') {
      return std::nullopt;
    }
  }

  unsigned long value = 0;
  const char* const first = text.data();
  const char* const last = first + text.size();
  const auto [ptr, ec] = std::from_chars(first, last, value);
  if (ec != std::errc{} || ptr != last || value > 65535UL) {
    return std::nullopt;  // overflow or out of range
  }
  return static_cast<std::uint16_t>(value);
}

std::optional<ServerMode> parse_server_mode(std::string_view text) {
  if (text == "thread-per-connection") {
    return ServerMode::kThreadPerConnection;
  }
  if (text == "thread-pool") {
    return ServerMode::kThreadPool;
  }
  if (text == "epoll") {
    return ServerMode::kEpoll;
  }
  return std::nullopt;
}

std::optional<std::uint16_t> parse_worker_count(std::string_view text) {
  const std::optional<std::uint16_t> count = parse_port(text);
  if (!count || *count == 0 || *count > 64) {
    return std::nullopt;
  }
  return count;
}

CliResult parse_cli(const std::vector<std::string>& args) {
  ServerConfig config;
  bool port_seen = false;
  bool bind_seen = false;
  bool mode_seen = false;
  bool workers_seen = false;

  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string& arg = args[i];

    if (arg == "-h" || arg == "--help") {
      return make_action(CliAction::kShowHelp);
    }
    if (arg == "-V" || arg == "--version") {
      return make_action(CliAction::kShowVersion);
    }

    if (arg == "-p" || arg == "--port" || arg.rfind(kPortEqualsPrefix, 0) == 0) {
      std::string_view value;
      if (arg == "-p" || arg == "--port") {
        if (i + 1 >= args.size()) {
          return make_error("option '" + arg + "' requires a value");
        }
        value = args[++i];
      } else {
        value = std::string_view(arg).substr(kPortEqualsPrefix.size());
      }

      if (port_seen) {
        return make_error("option '--port' was given more than once");
      }
      const std::optional<std::uint16_t> port = parse_port(value);
      if (!port) {
        return make_error("invalid port '" + std::string(value) +
                          "' (expected an integer from 0 to 65535)");
      }
      config.port = *port;
      port_seen = true;
      continue;
    }

    if (arg == "--bind" || arg.rfind(kBindEqualsPrefix, 0) == 0) {
      std::string_view value;
      if (arg == "--bind") {
        if (i + 1 >= args.size()) {
          return make_error("option '--bind' requires a value");
        }
        value = args[++i];
      } else {
        value = std::string_view(arg).substr(kBindEqualsPrefix.size());
      }
      if (bind_seen) {
        return make_error("option '--bind' was given more than once");
      }
      if (value.empty()) {
        return make_error("option '--bind' requires a non-empty IPv4 address");
      }
      config.bind_address = std::string(value);
      bind_seen = true;
      continue;
    }

    if (arg == "--mode" || arg.rfind(kModeEqualsPrefix, 0) == 0) {
      std::string_view value;
      if (arg == "--mode") {
        if (i + 1 >= args.size()) {
          return make_error("option '--mode' requires a value");
        }
        value = args[++i];
      } else {
        value = std::string_view(arg).substr(kModeEqualsPrefix.size());
      }
      if (mode_seen) {
        return make_error("option '--mode' was given more than once");
      }
      const std::optional<ServerMode> mode = parse_server_mode(value);
      if (!mode) {
        return make_error("unsupported server mode '" + std::string(value) +
                          "' (available: thread-per-connection, thread-pool, epoll)");
      }
      config.mode = *mode;
      mode_seen = true;
      continue;
    }

    if (arg == "--workers" || arg.rfind(kWorkersEqualsPrefix, 0) == 0) {
      std::string_view value;
      if (arg == "--workers") {
        if (i + 1 >= args.size()) {
          return make_error("option '--workers' requires a value");
        }
        value = args[++i];
      } else {
        value = std::string_view(arg).substr(kWorkersEqualsPrefix.size());
      }
      if (workers_seen) {
        return make_error("option '--workers' was given more than once");
      }
      const std::optional<std::uint16_t> workers = parse_worker_count(value);
      if (!workers) {
        return make_error("invalid worker count '" + std::string(value) +
                          "' (expected an integer from 1 to 64)");
      }
      config.worker_count = *workers;
      workers_seen = true;
      continue;
    }

    if (!arg.empty() && arg.front() == '-') {
      return make_error("unknown option '" + arg + "'");
    }
    return make_error("unexpected argument '" + arg + "'");
  }

  if (workers_seen && config.mode != ServerMode::kThreadPool) {
    return make_error("option '--workers' requires '--mode thread-pool'");
  }

  CliResult result;
  result.action = CliAction::kRun;
  result.config = config;
  return result;
}

std::string usage_text() {
  std::string text;
  text += "Usage: ";
  text += kProgramName;
  text += " [OPTIONS]\n\n";
  text += "Options:\n";
  text += "  -p, --port PORT   TCP port to listen on, 0-65535 (0 = ephemeral).\n";
  text += "                    Default: " + std::to_string(ServerConfig::kDefaultPort) + "\n";
  text += "      --bind ADDRESS Bind IPv4 address (default: 127.0.0.1).\n";
  text += "      --mode MODE    thread-per-connection, thread-pool, or epoll.\n";
  text += "      --workers N    Pool workers, 1-64 (default: 4).\n";
  text += "  -h, --help        Show this help and exit.\n";
  text += "  -V, --version     Show version and exit.\n\n";
  text += "Exit status: 0 success, 2 invalid command line.\n\n";
  text += "HTTP responses close each connection after one request. Supported\n";
  text += "routes: /, /health, and /hello.\n";
  return text;
}

std::string version_text() {
  std::string text(kProgramName);
  text += ' ';
  text += kVersion;
  return text;
}

}  // namespace srv
