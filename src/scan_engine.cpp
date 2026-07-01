#include "scan_engine.h"

#include "tuning_limits.h"

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
    const std::function<bool(uint32_t)> &tunerSetFrequency,
    const std::function<size_t(uint8_t *, size_t)> &tunerReadIQ,
    const std::function<void(const uint8_t *, size_t)> &writeIqCapture,
    const std::chrono::milliseconds &scanRetrySleep, uint8_t *iqBuffer,
    size_t sdrBufSamples, uint32_t iqSampleRate, int effectiveAppliedGainDb,
    double signalGainCompFactor,
    const Config::SDRSection &sdrConfig,
    const std::function<void(uint32_t, int)> &restoreAfterScan,
    const std::function<void()> &tunerFlush) {
  if (!m_active || !rtlConnected) {
    return false;
  }

  constexpr int kScanRetries = 1;
  constexpr int kFftAverages = 1;
  constexpr size_t kScanReadSamplesCap = 32768;
  // After a scan retune the source's buffered IQ is still from the previous
  // center. flushBuffers() drops that backlog; we then discard a settle window
  // to skip the PLL-lock + USB-in-flight samples captured before the new
  // center took effect, so the FFT only sees post-retune spectrum. Without
  // this the strong-station energy lands one sweep-step too high.
  const size_t kRetuneSettleSamples =
      std::max<size_t>(8192, iqSampleRate / 10); // ~100 ms
  constexpr float kUsableSpectrumFraction = 0.85f;
  constexpr float kCenterStepFraction = 0.60f;
  constexpr float kDcRejectHz = 2000.0f;
  constexpr double kWindowFloor = 1e-12;
  constexpr double kPowerFloor = 1e-20;

  const int startKHz = std::clamp(std::min(m_config.startKHz, m_config.stopKHz),
                                  static_cast<int>(fm_tuner::kFmBroadcastMinFreqKHz),
                                  static_cast<int>(fm_tuner::kFmBroadcastMaxFreqKHz));
  const int stopKHz = std::clamp(std::max(m_config.startKHz, m_config.stopKHz),
                                 static_cast<int>(fm_tuner::kFmBroadcastMinFreqKHz),
                                 static_cast<int>(fm_tuner::kFmBroadcastMaxFreqKHz));
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
  // Usable complex spectrum is Fs wide in total (±Fs/2). The half-span (from
  // tuned center to the edge of the usable window) is therefore
  // (Fs * kUsableSpectrumFraction) / 2 — but we also need to leave room for
  // the per-channel integration window (±halfChannelBW) so it does not wrap
  // across Nyquist and alias content from the opposite side of the band.
  const int64_t maxHalfSpanByFraction =
      static_cast<int64_t>((static_cast<double>(sampleRateHz) *
                            kUsableSpectrumFraction) /
                           2.0);
  const int64_t halfChannelBandwidthHz =
      static_cast<int64_t>(channelBandwidthHz) / 2;
  const int64_t maxHalfSpanByNyquist =
      std::max<int64_t>(1, (sampleRateHz / 2) - halfChannelBandwidthHz);
  const int64_t usableHalfSpanHz =
      std::min(maxHalfSpanByFraction, maxHalfSpanByNyquist);
  // Keep consecutive captures overlapping by at least ~25 %. This matters
  // when a wide scan bandwidth forces usableHalfSpanHz small enough that the
  // preferred Fs-fraction step would leave gaps between captures and push
  // every in-gap channel into the per-channel fallback path.
  const int64_t coverageCapHz =
      std::max<int64_t>(1, (usableHalfSpanHz * 3) / 2);
  const int64_t preferredStepHz =
      static_cast<int64_t>(static_cast<double>(sampleRateHz) *
                           kCenterStepFraction);
  const int64_t centerStepHz = std::max<int64_t>(
      static_cast<int64_t>(stepKHz) * 1000,
      std::min(preferredStepHz, coverageCapHz));
  // Place the first tune center one half-span above the requested start so
  // the left edge of the first captured span lands at startKHz. Likewise the
  // last allowed center sits one half-span above stopKHz, so a capture whose
  // left edge reaches stopKHz is still considered in bounds.
  int64_t centerHz =
      static_cast<int64_t>(startKHz) * 1000 + usableHalfSpanHz;
  const int64_t endCenterHz =
      static_cast<int64_t>(stopKHz) * 1000 + usableHalfSpanHz;
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

  auto estimateLevelsFromCapture =
      [&](int64_t tunedCenterHz, size_t samples, int firstChannel,
          int lastChannel, bool onlyMissing) -> bool {
    const size_t nfft = nearestPow2(std::min<size_t>(samples, 16384));
    if (nfft < 1024) {
      return false;
    }

    const float binHz = static_cast<float>(iqSampleRate) /
                        static_cast<float>(nfft);
    const int binHalf = std::max(
        1, static_cast<int>(std::lround((channelBandwidthHz * 0.5f) / binHz)));
    const int dcRejectBins = std::max(
        1, static_cast<int>(std::lround(kDcRejectHz / std::max(binHz, 1.0f))));
    const int guardBins = std::max(
        1, static_cast<int>(std::lround(6000.0f / std::max(binHz, 1.0f))));
    const int sideSpanBins = std::max(
        binHalf, static_cast<int>(std::lround((channelBandwidthHz * 0.35f) /
                                              std::max(binHz, 1.0f))));
    if (!m_fftState.ensureSize(nfft)) {
      return false;
    }

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
      const float w = m_fftState.window[i];
      m_fftState.fftIn[i] = {iRaw * w, qRaw * w};
    }

    fft_execute(m_fftState.plan);

    const int64_t spanLowHz = tunedCenterHz - usableHalfSpanHz;
    const int64_t spanHighHz = tunedCenterHz + usableHalfSpanHz;
    const double nfftNorm =
        static_cast<double>(nfft) * static_cast<double>(nfft);

    for (int ch = firstChannel; ch <= lastChannel; ch++) {
      if (onlyMissing &&
          std::isfinite(levelByChannel[static_cast<size_t>(ch)])) {
        continue;
      }

      const int freqKHz = startKHz + ch * stepKHz;
      const int64_t fHz = static_cast<int64_t>(freqKHz) * 1000;
      if (fHz < spanLowHz || fHz > spanHighHz) {
        continue;
      }

      const float relHz = static_cast<float>(fHz - tunedCenterHz);
      const int centerBin = static_cast<int>(
          std::lround((relHz / static_cast<float>(iqSampleRate)) *
                      static_cast<float>(nfft)));
      double channelSum = 0.0;
      int usedBins = 0;
      for (int b = centerBin - binHalf; b <= centerBin + binHalf; b++) {
        if (std::abs(b) <= dcRejectBins) {
          continue;
        }
        const int idx = binWrap(b, static_cast<int>(nfft));
        const float re = m_fftState.fftOut[static_cast<size_t>(idx)].real();
        const float im = m_fftState.fftOut[static_cast<size_t>(idx)].imag();
        channelSum += static_cast<double>(re) * static_cast<double>(re) +
                      static_cast<double>(im) * static_cast<double>(im);
        usedBins++;
      }
      if (usedBins <= 0) {
        continue;
      }

      double sideSum = 0.0;
      int sideBins = 0;
      for (int sign : {-1, 1}) {
        const int sideStart = centerBin + sign * (binHalf + guardBins);
        const int sideStop =
            centerBin + sign * (binHalf + guardBins + sideSpanBins);
        const int step = (sideStart <= sideStop) ? 1 : -1;
        for (int b = sideStart; b != sideStop + step; b += step) {
          if (std::abs(b) <= dcRejectBins) {
            continue;
          }
          const int idx = binWrap(b, static_cast<int>(nfft));
          const float re = m_fftState.fftOut[static_cast<size_t>(idx)].real();
          const float im = m_fftState.fftOut[static_cast<size_t>(idx)].imag();
          sideSum += static_cast<double>(re) * static_cast<double>(re) +
                     static_cast<double>(im) * static_cast<double>(im);
          sideBins++;
        }
      }

      const double bandPower = std::max(kPowerFloor, channelSum / nfftNorm);
      const double dbfs = 10.0 * std::log10(bandPower + kWindowFloor);
      const double compensatedDbfs =
          dbfs - static_cast<double>(effectiveAppliedGainDb) *
                     signalGainCompFactor +
          sdrConfig.signal_bias_db;
      const double safeNoisePerBin =
          std::max(kPowerFloor, (sideBins > 0)
                                    ? ((sideSum / static_cast<double>(sideBins)) /
                                       nfftNorm)
                                    : (bandPower / static_cast<double>(usedBins)));
      const double sidePower =
          std::max(kPowerFloor, safeNoisePerBin * static_cast<double>(usedBins));
      const double snrDb = std::max(
          0.0, 10.0 * std::log10((std::max(kPowerFloor, bandPower - sidePower) +
                                  kPowerFloor) /
                                 (sidePower + kPowerFloor)));
      const double safeCeilDbfs =
          std::max(sdrConfig.signal_ceil_dbfs, sdrConfig.signal_floor_dbfs + 1.0);
      const double clippedDbfs =
          std::clamp(compensatedDbfs, sdrConfig.signal_floor_dbfs, safeCeilDbfs);
      const float absLevel120 = static_cast<float>(
          ((clippedDbfs - sdrConfig.signal_floor_dbfs) /
           (safeCeilDbfs - sdrConfig.signal_floor_dbfs)) *
          120.0);
      // Report raw per-channel level to match XDR-GTK / TEF668x convention
      // (no FFT-SNR gate). Stations with low SNR due to adjacent-channel
      // spillover are still real signals worth seeing on the spectrum plot.
      (void)snrDb;
      const float level120 = absLevel120;
      levelByChannel[static_cast<size_t>(ch)] =
          std::max(levelByChannel[static_cast<size_t>(ch)], level120);
    }

    return true;
  };

  // Flush the stale pre-retune backlog, then read (and discard) a settle
  // window of fresh samples so the next measured read is clean.
  auto settleAfterRetune = [&]() {
    if (tunerFlush) {
      tunerFlush();
    }
    size_t discarded = 0;
    int emptyReads = 0;
    while (discarded < kRetuneSettleSamples && emptyReads < 4) {
      const size_t want =
          std::min(sdrBufSamples, kRetuneSettleSamples - discarded);
      const size_t got = tunerReadIQ(iqBuffer, want);
      if (got == 0) {
        emptyReads++;
        std::this_thread::sleep_for(scanRetrySleep);
        continue;
      }
      emptyReads = 0;
      discarded += got;
    }
  };

  for (; centerHz <= endCenterHz; centerHz += centerStepHz) {
    if (!shouldRun() || xdrServer.consumeScanCancel()) {
      m_active = false;
      break;
    }

    if (!tunerSetFrequency(static_cast<uint32_t>(centerHz))) {
      std::cerr << "[SCAN] warning: failed to retune to "
                << (centerHz / 1000) << " kHz; aborting scan\n";
      m_active = false;
      break;
    }
    // Drop the stale pre-retune backlog and let the tuner/NCO settle.
    settleAfterRetune();

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

      (void)estimateLevelsFromCapture(centerHz, samples, 0, channelCount - 1,
                                      false);
    }
  }

  // Fallback for uncovered channels so the client receives complete scan lines.
  // Batch contiguous uncovered channels into the largest span one retune can
  // cover, instead of retuning once per missing channel.
  for (int ch = 0; ch < channelCount;) {
    if (std::isfinite(levelByChannel[static_cast<size_t>(ch)])) {
      ch++;
      continue;
    }

    const int batchStart = ch;
    const int64_t batchStartHz =
        static_cast<int64_t>(startKHz + batchStart * stepKHz) * 1000;
    const int64_t batchMaxStopHz = batchStartHz + (usableHalfSpanHz * 2);
    int batchEnd = batchStart;
    while ((batchEnd + 1) < channelCount &&
           !std::isfinite(levelByChannel[static_cast<size_t>(batchEnd + 1)])) {
      const int64_t nextHz =
          static_cast<int64_t>(startKHz + (batchEnd + 1) * stepKHz) * 1000;
      if (nextHz > batchMaxStopHz) {
        break;
      }
      batchEnd++;
    }

    const int64_t batchEndHz =
        static_cast<int64_t>(startKHz + batchEnd * stepKHz) * 1000;
    const uint32_t batchCenterHz =
        static_cast<uint32_t>((batchStartHz + batchEndHz) / 2);

    if (!tunerSetFrequency(batchCenterHz)) {
      std::cerr << "[SCAN] warning: failed to retune to "
                << (batchCenterHz / 1000) << " kHz during fallback sampling\n";
      for (int missing = batchStart; missing <= batchEnd; missing++) {
        levelByChannel[static_cast<size_t>(missing)] = 0.0f;
      }
      ch = batchEnd + 1;
      continue;
    }
    settleAfterRetune();

    size_t samples = 0;
    for (int retries = 0; retries < kScanRetries && samples == 0; retries++) {
      samples =
          tunerReadIQ(iqBuffer, std::min(sdrBufSamples, static_cast<size_t>(4096)));
      if (samples == 0) {
        std::this_thread::sleep_for(scanRetrySleep);
      }
    }
    if (samples == 0) {
      for (int missing = batchStart; missing <= batchEnd; missing++) {
        levelByChannel[static_cast<size_t>(missing)] = 0.0f;
      }
      ch = batchEnd + 1;
      continue;
    }

    writeIqCapture(iqBuffer, samples);
    if (!estimateLevelsFromCapture(batchCenterHz, samples, batchStart, batchEnd,
                                   true)) {
      for (int missing = batchStart; missing <= batchEnd; missing++) {
        const int freqKHz = startKHz + missing * stepKHz;
        if (!tunerSetFrequency(static_cast<uint32_t>(freqKHz) * 1000U)) {
          std::cerr << "[SCAN] warning: failed to retune to " << freqKHz
                    << " kHz during per-channel fallback sampling\n";
          levelByChannel[static_cast<size_t>(missing)] = 0.0f;
          continue;
        }
        settleAfterRetune();
        size_t singleSamples = 0;
        for (int retries = 0; retries < kScanRetries && singleSamples == 0;
             retries++) {
          singleSamples = tunerReadIQ(
              iqBuffer, std::min(sdrBufSamples, static_cast<size_t>(4096)));
          if (singleSamples == 0) {
            std::this_thread::sleep_for(scanRetrySleep);
          }
        }
        if (singleSamples == 0) {
          levelByChannel[static_cast<size_t>(missing)] = 0.0f;
          continue;
        }
        writeIqCapture(iqBuffer, singleSamples);
        const SignalLevelResult signal = computeSignalLevel(
            iqBuffer, singleSamples, effectiveAppliedGainDb,
            signalGainCompFactor, sdrConfig.signal_bias_db,
            sdrConfig.signal_floor_dbfs, sdrConfig.signal_ceil_dbfs,
            iqSampleRate, channelBandwidthHz);
        levelByChannel[static_cast<size_t>(missing)] = signal.level120;
      }
    }

    for (int missing = batchStart; missing <= batchEnd; missing++) {
      if (!std::isfinite(levelByChannel[static_cast<size_t>(missing)])) {
        levelByChannel[static_cast<size_t>(missing)] = 0.0f;
      }
    }
    ch = batchEnd + 1;
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
