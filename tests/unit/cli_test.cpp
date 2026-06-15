#include "app/cli.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "common/version.hpp"

namespace srv {
namespace {

using Args = std::vector<std::string>;

// ---- parse_port ------------------------------------------------------------

TEST(ParsePort, AcceptsBoundaryValues) {
  EXPECT_EQ(parse_port("0"), std::optional<std::uint16_t>(0));
  EXPECT_EQ(parse_port("1"), std::optional<std::uint16_t>(1));
  EXPECT_EQ(parse_port("8080"), std::optional<std::uint16_t>(8080));
  EXPECT_EQ(parse_port("65535"), std::optional<std::uint16_t>(65535));
}

TEST(ParsePort, AcceptsLeadingZeros) {
  EXPECT_EQ(parse_port("0080"), std::optional<std::uint16_t>(80));
}

TEST(ParsePort, RejectsOutOfRange) {
  EXPECT_FALSE(parse_port("65536"));
  EXPECT_FALSE(parse_port("70000"));
  EXPECT_FALSE(parse_port("99999999999999999999999999"));
}

TEST(ParsePort, RejectsMalformedText) {
  EXPECT_FALSE(parse_port(""));
  EXPECT_FALSE(parse_port("-1"));
  EXPECT_FALSE(parse_port("+80"));
  EXPECT_FALSE(parse_port(" 80"));
  EXPECT_FALSE(parse_port("80 "));
  EXPECT_FALSE(parse_port("80x"));
  EXPECT_FALSE(parse_port("0x50"));
  EXPECT_FALSE(parse_port("8.0"));
  EXPECT_FALSE(parse_port("abc"));
}

// ---- parse_cli: successful runs ---------------------------------------------

TEST(ParseCli, NoArgumentsUsesDefaultPort) {
  const CliResult r = parse_cli({});
  EXPECT_EQ(r.action, CliAction::kRun);
  EXPECT_EQ(r.config.port, ServerConfig::kDefaultPort);
  EXPECT_TRUE(r.error.empty());
}

TEST(ParseCli, ShortPortOption) {
  const CliResult r = parse_cli(Args{"-p", "9000"});
  EXPECT_EQ(r.action, CliAction::kRun);
  EXPECT_EQ(r.config.port, 9000);
}

TEST(ParseCli, LongPortOptionWithSeparateValue) {
  const CliResult r = parse_cli(Args{"--port", "9001"});
  EXPECT_EQ(r.action, CliAction::kRun);
  EXPECT_EQ(r.config.port, 9001);
}

TEST(ParseCli, LongPortOptionWithEquals) {
  const CliResult r = parse_cli(Args{"--port=9002"});
  EXPECT_EQ(r.action, CliAction::kRun);
  EXPECT_EQ(r.config.port, 9002);
}

TEST(ParseCli, PortZeroIsAllowed) {
  const CliResult r = parse_cli(Args{"--port", "0"});
  EXPECT_EQ(r.action, CliAction::kRun);
  EXPECT_EQ(r.config.port, 0);
}

// ---- parse_cli: help and version --------------------------------------------

TEST(ParseCli, HelpShortAndLong) {
  EXPECT_EQ(parse_cli(Args{"-h"}).action, CliAction::kShowHelp);
  EXPECT_EQ(parse_cli(Args{"--help"}).action, CliAction::kShowHelp);
}

TEST(ParseCli, VersionShortAndLong) {
  EXPECT_EQ(parse_cli(Args{"-V"}).action, CliAction::kShowVersion);
  EXPECT_EQ(parse_cli(Args{"--version"}).action, CliAction::kShowVersion);
}

TEST(ParseCli, HelpTakesEffectWhenReachedLeftToRight) {
  EXPECT_EQ(parse_cli(Args{"--port", "80", "--help"}).action, CliAction::kShowHelp);
  EXPECT_EQ(parse_cli(Args{"--help", "--bogus"}).action, CliAction::kShowHelp);
  EXPECT_EQ(parse_cli(Args{"--bogus", "--help"}).action, CliAction::kError);
}

// ---- parse_cli: invalid input -------------------------------------------------

TEST(ParseCli, MissingPortValue) {
  const CliResult r = parse_cli(Args{"--port"});
  EXPECT_EQ(r.action, CliAction::kError);
  EXPECT_NE(r.error.find("requires a value"), std::string::npos);
  EXPECT_EQ(parse_cli(Args{"-p"}).action, CliAction::kError);
}

TEST(ParseCli, InvalidPortValues) {
  for (const char* bad : {"abc", "-1", "65536", "", "80x", "+80"}) {
    SCOPED_TRACE(bad);
    const CliResult r = parse_cli(Args{"--port", bad});
    EXPECT_EQ(r.action, CliAction::kError);
    EXPECT_NE(r.error.find("invalid port"), std::string::npos);
  }
  EXPECT_EQ(parse_cli(Args{"--port=70000"}).action, CliAction::kError);
  EXPECT_EQ(parse_cli(Args{"--port="}).action, CliAction::kError);
}

TEST(ParseCli, OptionValueIsNeverTreatedAsAnotherOption) {
  // "--help" here is the (invalid) value of -p, not a request for help.
  const CliResult r = parse_cli(Args{"-p", "--help"});
  EXPECT_EQ(r.action, CliAction::kError);
}

TEST(ParseCli, UnknownOption) {
  const CliResult r = parse_cli(Args{"--bogus"});
  EXPECT_EQ(r.action, CliAction::kError);
  EXPECT_NE(r.error.find("unknown option '--bogus'"), std::string::npos);
  EXPECT_EQ(parse_cli(Args{"-x"}).action, CliAction::kError);
  EXPECT_EQ(parse_cli(Args{"--"}).action, CliAction::kError);
  EXPECT_EQ(parse_cli(Args{"-p8080"}).action, CliAction::kError);
}

TEST(ParseCli, UnexpectedPositionalArgument) {
  const CliResult r = parse_cli(Args{"8080"});
  EXPECT_EQ(r.action, CliAction::kError);
  EXPECT_NE(r.error.find("unexpected argument '8080'"), std::string::npos);
  EXPECT_EQ(parse_cli(Args{""}).action, CliAction::kError);
}

TEST(ParseCli, DuplicatePortIsRejected) {
  const CliResult r = parse_cli(Args{"--port", "80", "-p", "81"});
  EXPECT_EQ(r.action, CliAction::kError);
  EXPECT_NE(r.error.find("more than once"), std::string::npos);
}

// ---- text helpers ---------------------------------------------------------------

TEST(Text, UsageMentionsAllOptions) {
  const std::string usage = usage_text();
  EXPECT_NE(usage.find("--port"), std::string::npos);
  EXPECT_NE(usage.find("--help"), std::string::npos);
  EXPECT_NE(usage.find("--version"), std::string::npos);
  EXPECT_NE(usage.find("8080"), std::string::npos);
}

TEST(Text, VersionContainsProgramNameAndVersion) {
  EXPECT_EQ(version_text(), std::string(kProgramName) + " " + std::string(kVersion));
}

}  // namespace
}  // namespace srv
