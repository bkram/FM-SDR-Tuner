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
  uint32_t freqKHz = 87500;
  int gain = -1;
  std::string wavFile;
  std::string mpxWavFile;
  // 0 = match the post-decimation IQ rate (no resample, current default).
  // Otherwise the MPX is resampled to this rate before being written.
  // 192000 Hz is a common target for downstream RDS / spectrum analysis tools
  // and for feeding FM exciters that accept raw MPX.
  uint32_t mpxWavSampleRate = 0;
  // Gain (dB) applied to the MPX WAV / MPX audio outputs. The demod
  // calibrates 75 kHz deviation to 1.0 full scale, and real broadcasts
  // deviate to and beyond 75 kHz, so 0 dB hard-clips on peaks. The -6 dB
  // default puts full scale at 150 kHz deviation: no clipping on any real
  // signal, and a documented calibration for downstream analyzers.
  float mpxGainDb = -6.0f;
  // Live MPX -> audio device routing (intended for virtual loopback like
  // BlackHole / snd-aloop, or for direct line-in into an FM exciter).
  bool mpxAudioEnabled = false;
  std::string mpxAudioDevice;
  std::string iqFile;
  bool enableSpeaker = false;
  std::string audioDevice;
  std::string xdrPassword;
  bool xdrGuestMode = false;
  uint16_t xdrPort = 7373;
  bool autoReconnect = true;
  bool autoStart = false;
  bool lowLatencyIq = false;
  bool verboseLogging = true;
  std::string stereoBlendOverride; // empty = use config; else "soft"|"normal"|"aggressive"
  // One-shot band-sweep / calibration mode. When true, main() opens the
  // RTL-SDR, sweeps 87.5–108 MHz, prints a per-frequency table + recommended
  // INI values, and exits without starting the normal tuner pipeline.
  bool calibrate = false;
};

struct AppParseResult {
  AppParseOutcome outcome = AppParseOutcome::ExitFailure;
  AppOptions options;
};

void printUsage(const char *prog);
AppParseResult parseAppOptions(int argc, char *argv[], int inputRate);

#endif
