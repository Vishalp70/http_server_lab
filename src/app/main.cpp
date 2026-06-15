#include <iostream>
#include <string>
#include <vector>

#include "app/cli.hpp"
#include "common/version.hpp"

namespace {

constexpr int kExitOk = 0;
constexpr int kExitUsage = 2;

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

  // Milestone 1 stops here on purpose: no socket is created yet.
  std::cout << srv::version_text() << ": configuration OK (port " << cli.config.port
            << "). HTTP server not implemented yet.\n";
  return kExitOk;
}
