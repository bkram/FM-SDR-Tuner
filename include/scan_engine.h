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
  struct FftState {
    ~FftState() {
      if (plan != nullptr) {
        fft_destroy_plan(plan);
      }
    }

    bool ensureSize(size_t requestedNfft) {
      if (nfft == requestedNfft && plan != nullptr) {
        return true;
      }
      if (plan != nullptr) {
        fft_destroy_plan(plan);
        plan = nullptr;
      }
      nfft = requestedNfft;
      fftIn.assign(nfft, {});
      fftOut.assign(nfft, {});
      window.resize(nfft);
      if (nfft > 1) {
        for (size_t i = 0; i < nfft; i++) {
          window[i] = 0.5f - 0.5f * std::cos(
                                   static_cast<float>(2.0 * 3.14159265358979323846) *
                                   static_cast<float>(i) /
                                   static_cast<float>(nfft - 1));
        }
      } else if (nfft == 1) {
        window[0] = 1.0f;
      }
      plan = fft_create_plan(static_cast<unsigned int>(nfft), fftIn.data(),
                             fftOut.data(), LIQUID_FFT_FORWARD, 0);
      return plan != nullptr;
    }

    size_t nfft = 0;
    std::vector<std::complex<float>> fftIn;
    std::vector<std::complex<float>> fftOut;
    std::vector<float> window;
    fftplan plan = nullptr;
  };

  bool m_active;
  XDRServer::ScanConfig m_config;
  uint32_t m_restoreFreqHz;
  int m_restoreBandwidthHz;
  FftState m_fftState;
};

#endif
