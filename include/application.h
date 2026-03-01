#ifndef APPLICATION_H
#define APPLICATION_H

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>

#include "app_options.h"

class AudioOutput;
class CPUFeatures;
class TunerSession;
class XDRServer;

class Application {
public:
  explicit Application(const AppOptions &options) : m_options(options) {}

  int run();

private:
  void logStartup(const CPUFeatures &cpu) const;
  bool initAudioOutput(AudioOutput &audioOut, TunerSession &tunerSession,
                       std::atomic<int> &requestedVolume) const;
  FILE *openIqCapture(AudioOutput &audioOut, TunerSession &tunerSession) const;
  bool readIqSamples(
      const std::function<size_t(uint8_t *, size_t)> &tunerReadIQ,
      const std::function<void(const uint8_t *, size_t)> &writeIqCapture,
      uint8_t *iqBuffer, size_t sdrBufSamples,
      const std::chrono::milliseconds &noDataSleep, TunerSession &tunerSession,
      bool verboseLogging, size_t &samples) const;
  void shutdownResources(AudioOutput &audioOut, FILE *&iqHandle,
                         XDRServer &xdrServer,
                         TunerSession &tunerSession) const;

  AppOptions m_options;
};

#endif
