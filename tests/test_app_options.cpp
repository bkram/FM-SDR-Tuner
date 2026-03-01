#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string>
#include <vector>

#include "app_options.h"

namespace {

std::vector<char *> makeArgv(std::vector<std::string> &args) {
  std::vector<char *> argv;
  argv.reserve(args.size());
  for (std::string &s : args) {
    argv.push_back(s.data());
  }
  return argv;
}

} // namespace

TEST_CASE("App options parser handles help", "[app_options]") {
  std::vector<std::string> args = {"fm-sdr-tuner", "--help"};
  std::vector<char *> argv = makeArgv(args);
  const AppParseResult result =
      parseAppOptions(static_cast<int>(argv.size()), argv.data(), 256000);
  REQUIRE(result.outcome == AppParseOutcome::ExitSuccess);
}

TEST_CASE("App options parser normalizes tuner source aliases", "[app_options]") {
  std::vector<std::string> args = {"fm-sdr-tuner", "--source", "tcp", "-s"};
  std::vector<char *> argv = makeArgv(args);
  const AppParseResult result =
      parseAppOptions(static_cast<int>(argv.size()), argv.data(), 256000);
  REQUIRE(result.outcome == AppParseOutcome::Run);
  REQUIRE(result.options.tunerSource == "rtl_tcp");
}

TEST_CASE("App options parser rejects invalid IQ rate", "[app_options]") {
  std::vector<std::string> args = {"fm-sdr-tuner", "--iq-rate", "123456"};
  std::vector<char *> argv = makeArgv(args);
  const AppParseResult result =
      parseAppOptions(static_cast<int>(argv.size()), argv.data(), 256000);
  REQUIRE(result.outcome == AppParseOutcome::ExitFailure);
}

TEST_CASE("App options parser toggles low-latency IQ flag", "[app_options]") {
  std::vector<std::string> args = {"fm-sdr-tuner", "--low-latency-iq",
                                   "--no-low-latency-iq", "-s"};
  std::vector<char *> argv = makeArgv(args);
  const AppParseResult result =
      parseAppOptions(static_cast<int>(argv.size()), argv.data(), 256000);
  REQUIRE(result.outcome == AppParseOutcome::Run);
  REQUIRE_FALSE(result.options.lowLatencyIq);
}

TEST_CASE("App options parser reads tcp host and port", "[app_options]") {
  std::vector<std::string> args = {"fm-sdr-tuner", "--tcp", "192.168.1.2:4321",
                                   "-s"};
  std::vector<char *> argv = makeArgv(args);
  const AppParseResult result =
      parseAppOptions(static_cast<int>(argv.size()), argv.data(), 256000);
  REQUIRE(result.outcome == AppParseOutcome::Run);
  REQUIRE(result.options.tcpHost == "192.168.1.2");
  REQUIRE(result.options.tcpPort == 4321);
}

TEST_CASE("App options parser rejects missing output selection",
          "[app_options]") {
  std::vector<std::string> args = {"fm-sdr-tuner", "--source", "rtl_sdr"};
  std::vector<char *> argv = makeArgv(args);
  const AppParseResult result =
      parseAppOptions(static_cast<int>(argv.size()), argv.data(), 256000);
  REQUIRE(result.outcome == AppParseOutcome::ExitFailure);
}

TEST_CASE("App options parser accepts IQ capture as output", "[app_options]") {
  std::vector<std::string> args = {"fm-sdr-tuner", "--iq", "/tmp/capture.iq"};
  std::vector<char *> argv = makeArgv(args);
  const AppParseResult result =
      parseAppOptions(static_cast<int>(argv.size()), argv.data(), 256000);
  REQUIRE(result.outcome == AppParseOutcome::Run);
  REQUIRE(result.options.iqFile == "/tmp/capture.iq");
}
