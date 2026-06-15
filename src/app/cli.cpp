#include "app/cli.hpp"

#include <charconv>
#include <cstddef>
#include <system_error>
#include <utility>

#include "common/version.hpp"

namespace srv {
namespace {

constexpr std::string_view kPortEqualsPrefix = "--port=";

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

CliResult parse_cli(const std::vector<std::string>& args) {
  ServerConfig config;
  bool port_seen = false;

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

    if (!arg.empty() && arg.front() == '-') {
      return make_error("unknown option '" + arg + "'");
    }
    return make_error("unexpected argument '" + arg + "'");
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
  text += "  -h, --help        Show this help and exit.\n";
  text += "  -V, --version     Show version and exit.\n\n";
  text += "Exit status: 0 success, 2 invalid command line.\n\n";
  text += "Milestone 1: the command line is parsed and validated, but no socket\n";
  text += "is opened yet.\n";
  return text;
}

std::string version_text() {
  std::string text(kProgramName);
  text += ' ';
  text += kVersion;
  return text;
}

}  // namespace srv
