#include "scan_engine.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <thread>
#include <vector>

#include "dsp/liquid_primitives.h"

#include "signal_level.h"

ScanEngine::ScanEngine()
    : m_active(false), m_restoreFreqHz(0), m_restoreBandwidthHz(0) {}

void ScanEngine::handleControl(
    XDRServer &xdrServer, uint32_t currentFreqHz, int currentBandwidthHz,
    bool rtlConnected, bool verboseLogging, std::atomic<int> &requestedBandwidthHz,
    std::atomic<bool> &pendingBandwidth,
    const std::function<void(uint32_t, int)> &restoreAfterScan) {
  XDRServer::ScanConfig newScanConfig;
  if (xdrServer.consumeScanStart(newScanConfig)) {
    m_config = newScanConfig;
    m_active = true;
    m_restoreFreqHz = currentFreqHz;
    m_restoreBandwidthHz = currentBandwidthHz;
    if (m_config.bandwidthHz > 0) {
      requestedBandwidthHz = m_config.bandwidthHz;
      pendingBandwidth = true;
    }
    if (verboseLogging) {
      std::cout << "[SCAN] start "
                << "from=" << m_config.startKHz << " to=" << m_config.stopKHz
                << " step=" << m_config.stepKHz << " bw=" << m_config.bandwidthHz
                << " mode=" << (m_config.continuous ? "continuous" : "single")
                << "\n";
    }
  }

  if (xdrServer.consumeScanCancel()) {
    const bool wasActive = m_active;
    m_active = false;
    if (verboseLogging) {
      std::cout << "[SCAN] cancel requested\n";
    }
    if (wasActive && rtlConnected) {
      restoreAfterScan(m_restoreFreqHz, m_restoreBandwidthHz);
    }
  }
}

bool ScanEngine::runIfActive(
    XDRServer &xdrServer, bool rtlConnected, const std::function<bool()> &shouldRun,
    const std::function<void(uint32_t)> &tunerSetFrequency,
    const std::function<size_t(uint8_t *, size_t)> &tunerReadIQ,
    const std::function<void(const uint8_t *, size_t)> &writeIqCapture,
    const std::chrono::milliseconds &scanRetrySleep, uint8_t *iqBuffer,
    size_t sdrBufSamples, uint32_t iqSampleRate, int effectiveAppliedGainDb,
    double signalGainCompFactor,
    const Config::SDRSection &sdrConfig,
    const std::function<void(uint32_t, int)> &restoreAfterScan) {
  if (!m_active || !rtlConnected) {
    return false;
  }

  constexpr int kScanRetries = 2;
  constexpr int kFftAverages = 2;
  constexpr size_t kScanReadSamplesCap = 32768;
  constexpr size_t kRetuneDiscardSamples = 2048;
  constexpr float kUsableSpectrumFraction = 0.45f;
  constexpr float kCenterStepFraction = 0.75f;
  constexpr float kDcRejectHz = 4000.0f;
  constexpr float kPi = 3.14159265358979323846f;
  constexpr double kWindowFloor = 1e-12;
  constexpr double kPowerFloor = 1e-20;

  const int startKHz = std::min(m_config.startKHz, m_config.stopKHz);
  const int stopKHz = std::max(m_config.startKHz, m_config.stopKHz);
  const int stepKHz = std::max(5, m_config.stepKHz);
  const int channelBandwidthHz =
      std::clamp((m_config.bandwidthHz > 0) ? m_config.bandwidthHz : 56000,
                 10000, 200000);
  const int channelCount = ((stopKHz - startKHz) / stepKHz) + 1;
  std::vector<float> levelByChannel(
      static_cast<size_t>(channelCount),
      -std::numeric_limits<float>::infinity());

  if (iqSampleRate == 0) {
    if (!m_config.continuous || !m_active) {
      m_active = false;
      restoreAfterScan(m_restoreFreqHz, m_restoreBandwidthHz);
    }
    return true;
  }

  const int64_t sampleRateHz = static_cast<int64_t>(iqSampleRate);
  const int64_t usableHalfSpanHz =
      static_cast<int64_t>(static_cast<double>(sampleRateHz) *
                           kUsableSpectrumFraction);
  const int64_t centerStepHz = std::max<int64_t>(
      static_cast<int64_t>(stepKHz) * 1000,
      static_cast<int64_t>(static_cast<double>(sampleRateHz) *
                           kCenterStepFraction));
  int64_t centerHz = static_cast<int64_t>(startKHz) * 1000 +
                     (usableHalfSpanHz / 2);
  const int64_t endCenterHz =
      static_cast<int64_t>(stopKHz) * 1000 + (usableHalfSpanHz / 2);
  const size_t scanReadSamples =
      std::min(sdrBufSamples, std::max<size_t>(8192, kScanReadSamplesCap));

  auto nearestPow2 = [](size_t n) -> size_t {
    size_t p = 1;
    while ((p << 1U) <= n) {
      p <<= 1U;
    }
    return p;
  };

  auto binWrap = [](int idx, int nfft) -> int {
    int wrapped = idx % nfft;
    if (wrapped < 0) {
      wrapped += nfft;
    }
    return wrapped;
  };

  for (; centerHz <= endCenterHz; centerHz += centerStepHz) {
    if (!shouldRun() || xdrServer.consumeScanCancel()) {
      m_active = false;
      break;
    }

    tunerSetFrequency(static_cast<uint32_t>(centerHz));
    // Discard one short read after retune to let tuner/NCO settle.
    (void)tunerReadIQ(iqBuffer, std::min(sdrBufSamples, kRetuneDiscardSamples));

    for (int avg = 0; avg < kFftAverages; avg++) {
      size_t samples = 0;
      for (int retries = 0; retries < kScanRetries && samples == 0; retries++) {
        samples = tunerReadIQ(iqBuffer, scanReadSamples);
        if (samples == 0) {
          std::this_thread::sleep_for(scanRetrySleep);
        }
      }
      if (samples == 0) {
        continue;
      }

      writeIqCapture(iqBuffer, samples);

      const size_t nfft = nearestPow2(std::min<size_t>(samples, 16384));
      if (nfft < 1024) {
        continue;
      }
      const float binHz = static_cast<float>(iqSampleRate) /
                          static_cast<float>(nfft);
      const int binHalf = std::max(
          1, static_cast<int>(std::lround((channelBandwidthHz * 0.5f) / binHz)));
      const int dcRejectBins = std::max(
          1, static_cast<int>(std::lround(kDcRejectHz / std::max(binHz, 1.0f))));
      std::vector<std::complex<float>> fftIn(nfft);
      std::vector<std::complex<float>> fftOut(nfft);
      double meanI = 0.0;
      double meanQ = 0.0;
      for (size_t i = 0; i < nfft; i++) {
        meanI += (static_cast<int>(iqBuffer[i * 2]) - 127.5) * (1.0 / 127.5);
        meanQ += (static_cast<int>(iqBuffer[i * 2 + 1]) - 127.5) * (1.0 / 127.5);
      }
      meanI /= static_cast<double>(nfft);
      meanQ /= static_cast<double>(nfft);
      for (size_t i = 0; i < nfft; i++) {
        const float iRaw =
            static_cast<float>((static_cast<int>(iqBuffer[i * 2]) - 127.5) *
                                   (1.0 / 127.5) -
                               meanI);
        const float qRaw = static_cast<float>(
            (static_cast<int>(iqBuffer[i * 2 + 1]) - 127.5) * (1.0 / 127.5) -
            meanQ);
        const float w = 0.5f - 0.5f * std::cos(
                                     static_cast<float>(2.0f * kPi) *
                                     static_cast<float>(i) /
                                     static_cast<float>(nfft - 1));
        fftIn[i] = {iRaw * w, qRaw * w};
      }

      fftplan plan =
          fft_create_plan(static_cast<unsigned int>(nfft), fftIn.data(),
                          fftOut.data(), LIQUID_FFT_FORWARD, 0);
      if (!plan) {
        continue;
      }
      fft_execute(plan);
      fft_destroy_plan(plan);

      const int64_t spanLowHz = centerHz - usableHalfSpanHz;
      const int64_t spanHighHz = centerHz + usableHalfSpanHz;
      const double nfftNorm =
          static_cast<double>(nfft) * static_cast<double>(nfft);

      for (int ch = 0; ch < channelCount; ch++) {
        const int freqKHz = startKHz + ch * stepKHz;
        const int64_t fHz = static_cast<int64_t>(freqKHz) * 1000;
        if (fHz < spanLowHz || fHz > spanHighHz) {
          continue;
        }

        const float relHz = static_cast<float>(fHz - centerHz);
        const int centerBin = static_cast<int>(
            std::lround((relHz / static_cast<float>(iqSampleRate)) *
                        static_cast<float>(nfft)));
        double sum = 0.0;
        int usedBins = 0;
        for (int b = centerBin - binHalf; b <= centerBin + binHalf; b++) {
          if (std::abs(b) <= dcRejectBins) {
            continue;
          }
          const int idx = binWrap(b, static_cast<int>(nfft));
          const float re = fftOut[static_cast<size_t>(idx)].real();
          const float im = fftOut[static_cast<size_t>(idx)].imag();
          sum += static_cast<double>(re) * static_cast<double>(re) +
                 static_cast<double>(im) * static_cast<double>(im);
          usedBins++;
        }
        if (usedBins <= 0) {
          continue;
        }
        const double bandPower = std::max(kPowerFloor, sum / nfftNorm);
        const double dbfs = 10.0 * std::log10(bandPower + kWindowFloor);
        const double compensatedDbfs =
            dbfs - static_cast<double>(effectiveAppliedGainDb) *
                       signalGainCompFactor +
            sdrConfig.signal_bias_db;
        const double safeCeilDbfs =
            std::max(sdrConfig.signal_ceil_dbfs, sdrConfig.signal_floor_dbfs + 1.0);
        const double clippedDbfs =
            std::clamp(compensatedDbfs, sdrConfig.signal_floor_dbfs, safeCeilDbfs);
        const float level120 = static_cast<float>(
            ((clippedDbfs - sdrConfig.signal_floor_dbfs) /
             (safeCeilDbfs - sdrConfig.signal_floor_dbfs)) *
            120.0);
        levelByChannel[static_cast<size_t>(ch)] =
            std::max(levelByChannel[static_cast<size_t>(ch)], level120);
      }
    }
  }

  // Fallback for uncovered channels so the client receives complete scan lines.
  for (int ch = 0; ch < channelCount; ch++) {
    if (std::isfinite(levelByChannel[static_cast<size_t>(ch)])) {
      continue;
    }
    const int freqKHz = startKHz + ch * stepKHz;
    tunerSetFrequency(static_cast<uint32_t>(freqKHz) * 1000U);
    size_t samples = 0;
    for (int retries = 0; retries < kScanRetries && samples == 0; retries++) {
      samples = tunerReadIQ(iqBuffer, std::min(sdrBufSamples, static_cast<size_t>(4096)));
      if (samples == 0) {
        std::this_thread::sleep_for(scanRetrySleep);
      }
    }
    if (samples == 0) {
      levelByChannel[static_cast<size_t>(ch)] = 0.0f;
      continue;
    }
    writeIqCapture(iqBuffer, samples);
    const SignalLevelResult signal = computeSignalLevel(
        iqBuffer, samples, effectiveAppliedGainDb, signalGainCompFactor,
        sdrConfig.signal_bias_db, sdrConfig.signal_floor_dbfs,
        sdrConfig.signal_ceil_dbfs);
    levelByChannel[static_cast<size_t>(ch)] = signal.level120;
  }

  std::ostringstream scanLine;
  for (int ch = 0; ch < channelCount; ch++) {
    const float rfLevel = levelByChannel[static_cast<size_t>(ch)];
    if (!std::isfinite(rfLevel)) {
      continue;
    }
    const int f = startKHz + ch * stepKHz;
    scanLine << f << "=" << std::fixed << std::setprecision(1) << rfLevel
             << ",";
  }

  if (!scanLine.str().empty()) {
    xdrServer.pushScanLine(scanLine.str());
  }

  if (!m_config.continuous || !m_active) {
    m_active = false;
    restoreAfterScan(m_restoreFreqHz, m_restoreBandwidthHz);
  }
  return true;
}
