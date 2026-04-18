#ifndef TUNER_SESSION_H
#define TUNER_SESSION_H

#include <cstdint>
#include <functional>
#include <string>

#include "tuner_controller.h"

class TunerSession {
public:
  struct Params {
    bool useDirectRtlSdr = false;
    bool verboseLogging = false;
    uint32_t rtlDeviceIndex = 0;
    std::string tcpHost;
    uint16_t tcpPort = 0;
    uint32_t initialFreqKHz = 0;
    uint32_t iqSampleRate = 0;
    int freqCorrectionPpm = 0;
    bool autoReconnect = false;
  };

  TunerSession(TunerController &tuner, bool &rtlConnected, Params params,
               std::function<uint32_t()> requestedFrequencyHz,
               std::function<int()> requestedAGCMode,
               std::function<int()> requestedCustomGain,
               std::function<void(const char *reason)> applyRtlGainAndAgc);

  const char *tunerName() const;
  void connect();
  void disconnect();
  void resetReadFailures();
  void noteReadFailureAndMaybeReconnect();

private:
  TunerController &m_tuner;
  bool &m_rtlConnected;
  Params m_params;
  std::function<uint32_t()> m_requestedFrequencyHz;
  std::function<int()> m_requestedAGCMode;
  std::function<int()> m_requestedCustomGain;
  std::function<void(const char *reason)> m_applyRtlGainAndAgc;
  int m_consecutiveReadFailures = 0;
};

#endif
