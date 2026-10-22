#ifndef SCAN_ENGINE_H
#define SCAN_ENGINE_H

#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include "config.h"
#include "dsp/liquid_primitives.h"
#include "xdr_server.h"

class ScanEngine {
public:
  ScanEngine();

  void handleControl(XDRServer &xdrServer, uint32_t currentFreqHz,
                     int currentBandwidthHz, bool rtlConnected,
                     bool verboseLogging, std::atomic<int> &requestedBandwidthHz,
                     std::atomic<bool> &pendingBandwidth,
                     const std::function<void(uint32_t, int)> &restoreAfterScan);

  bool runIfActive(
      XDRServer &xdrServer, bool rtlConnected,
      const std::function<bool()> &shouldRun,
      const std::function<bool(uint32_t)> &tunerSetFrequency,
      const std::function<size_t(uint8_t *, size_t)> &tunerReadIQ,
      const std::function<void(const uint8_t *, size_t)> &writeIqCapture,
      const std::chrono::milliseconds &scanRetrySleep, uint8_t *iqBuffer,
      size_t sdrBufSamples, uint32_t iqSampleRate, int effectiveAppliedGainDb,
      double signalGainCompFactor, const Config::SDRSection &sdrConfig,
      const std::function<void(uint32_t, int)> &restoreAfterScan,
      const std::function<void()> &tunerFlush = {});

  // True while a scan sweep is in progress (single or continuous). Used by the
  // control loop to drive an SDRplay wide-bandwidth scan mode around the sweep.
  bool isActive() const { return m_active; }

private:
  bool m_active;
  XDRServer::ScanConfig m_config;
  uint32_t m_restoreFreqHz;
  int m_restoreBandwidthHz;
};

#endif
