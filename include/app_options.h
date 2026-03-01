#ifndef APP_OPTIONS_H
#define APP_OPTIONS_H

#include <cstdint>
#include <string>

#include "config.h"

enum class AppParseOutcome { Run = 0, ExitSuccess = 1, ExitFailure = 2 };

struct AppOptions {
  Config config;
  std::string configPath;
  std::string tcpHost;
  uint16_t tcpPort = 1234;
  uint32_t iqSampleRate = 256000;
  std::string tunerSource = "rtl_sdr";
  uint32_t rtlDeviceIndex = 0;
  uint32_t freqKHz = 88600;
  int gain = -1;
  std::string wavFile;
  std::string iqFile;
  bool enableSpeaker = false;
  std::string audioDevice;
  std::string xdrPassword;
  bool xdrGuestMode = false;
  uint16_t xdrPort = 7373;
  bool autoReconnect = true;
  bool lowLatencyIq = false;
  bool verboseLogging = true;
};

struct AppParseResult {
  AppParseOutcome outcome = AppParseOutcome::ExitFailure;
  AppOptions options;
};

void printUsage(const char *prog);
AppParseResult parseAppOptions(int argc, char *argv[], int inputRate);

#endif
