#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/config.hpp"

namespace srv {

// What the process should do after command-line parsing.
enum class CliAction {
  kRun,          // arguments valid: start the server with `config`
  kShowHelp,     // print usage and exit 0
  kShowVersion,  // print version and exit 0
  kError,        // arguments invalid: print `error` and exit with a usage error
};

struct CliResult {
  CliAction action = CliAction::kRun;
  ServerConfig config;  // meaningful only when action == kRun
  std::string error;    // non-empty only when action == kError
};

// Parses command-line arguments. `args` must NOT include argv[0].
// Pure function: performs no I/O and never exits, so it is unit-testable.
// Arguments are processed left to right; --help and --version take effect as
// soon as they are reached.
CliResult parse_cli(const std::vector<std::string>& args);

// Strict port parser: decimal digits only, value 0..65535. Rejects empty
// text, signs, whitespace, trailing characters and overflow.
std::optional<std::uint16_t> parse_port(std::string_view text);

std::string usage_text();
std::string version_text();

}  // namespace srv
