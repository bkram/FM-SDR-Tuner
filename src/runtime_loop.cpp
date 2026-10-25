#include "runtime_loop.h"
#include "signal_level.h"

#include "auto_gain_policy.h"
#include "retune_policy.h"

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <thread>

namespace runtime_loop {

namespace {

} // namespace

bool handleControlAndScan(
    ScanEngine &scanEngine, XDRServer &xdrServer,
    std::atomic<uint32_t> &requestedFrequencyHz,
    std::atomic<bool> &pendingFrequency, std::atomic<bool> &pendingGain,
    std::atomic<bool> &pendingAGC, std::atomic<int> &requestedBandwidthHz,
    std::atomic<bool> &pendingBandwidth, std::atomic<int> &appliedBandwidthHz,
    bool rtlConnected, bool verboseLogging, AudioOutput &audioOut,
    fm_tuner::dsp::Runtime &dspRuntime, RdsWorker &rdsWorker,
    DspPipeline &dspPipeline, size_t kRetuneMuteSamples,
    size_t &retuneMuteSamplesRemaining, size_t &retuneMuteTotalSamples,
    const std::function<void(const char *reason)> &applyRtlGainAndAgc,
    const std::function<bool(uint32_t)> &tunerSetFrequency,
    const std::function<uint32_t()> &currentTunerFrequencyHz,
    const std::function<size_t(uint8_t *, size_t)> &tunerReadIQ,
    const std::function<void(const uint8_t *, size_t)> &writeIqCapture,
    const std::chrono::milliseconds &scanRetrySleep, uint8_t *iqBuffer,
    size_t sdrBufSamples, uint32_t iqSampleRate, int effectiveAppliedGainDb,
    double signalGainCompFactor, const Config::SDRSection &sdrConfig,
    const std::function<void(uint32_t, int)> &restoreAfterScan,
    const std::function<bool()> &shouldRun,
    const std::function<uint32_t(bool)> &setScanWideMode, bool &scanWideActive,
    uint32_t &scanWideRate, const std::function<void()> &tunerFlush) {
  scanEngine.handleControl(xdrServer, requestedFrequencyHz.load(),
                           appliedBandwidthHz.load(), rtlConnected,
                           verboseLogging, requestedBandwidthHz, pendingBandwidth,
                           restoreAfterScan);

  if (rtlConnected && pendingFrequency.exchange(false)) {
    const uint32_t targetFrequencyHz = requestedFrequencyHz.load();
    const uint32_t appliedFrequencyHz =
        currentTunerFrequencyHz ? currentTunerFrequencyHz() : 0U;
    if (fm_tuner::isRedundantRetune(targetFrequencyHz, appliedFrequencyHz)) {
      // The tuner is already on the requested frequency. Do NOT retune: a
      // retune flushes the audio queue, resets the DSP and mutes the output
      // while the demodulator settles, and the listener hears the programme
      // restart -- the station "repeats" itself. Clients re-send the
      // frequency they are already on routinely (xdr-gtk on every click of
      // the tuned entry, the REST panel on every poll that carries a
      // frequency), so this is the common case. Re-assert the gain/AGC
      // registers only: that is inaudible and keeps the "a retune heals
      // drifted tuner registers" property.
      xdrServer.setFrequencyState(targetFrequencyHz);
      applyRtlGainAndAgc("retune/reassert(same frequency)");
      if (verboseLogging) {
        std::cout << "[XDR] already tuned to " << (targetFrequencyHz / 1000)
                  << " kHz; no retune\n";
      }
    } else if (tunerSetFrequency(targetFrequencyHz)) {
      // Reflect the applied frequency in the XDR server state regardless of who
      // requested it (XDR command, REST API, scan restore, ...). The XDR/FM-DX
      // command handlers set it themselves, but REST/internal retunes go only
      // through the pending-frequency path, so without this an XDR client (e.g.
      // FM-DX-Webserver) would report a stale frequency after a REST change.
      xdrServer.setFrequencyState(targetFrequencyHz);
      // Re-assert the full gain/AGC/mode device state on every retune. A plain
      // retune otherwise reprograms ONLY the center frequency; gain-mode, gain
      // and AGC registers are written once at connect(). Field reports show
      // reception degrading over hours with "retune doesn't help, restart
      // does" — a restart heals because reconnect reprograms everything. Make
      // a retune re-establish the same reprogrammable state so silently lost
      // or drifted tuner registers are corrected without a restart.
      applyRtlGainAndAgc("retune/reassert");
      audioOut.clearRealtimeQueue();
      dspRuntime.reset(fm_tuner::dsp::ResetReason::Retune);
      // The meter tracks the DC offset across blocks; the previous station's
      // estimate would otherwise linger in the new one's reading.
      resetSignalLevelDcTracking();
      retuneMuteSamplesRemaining = kRetuneMuteSamples;
      retuneMuteTotalSamples = kRetuneMuteSamples;
      rdsWorker.requestReset();
    } else {
      std::cerr << "[SDR] warning: failed to retune to "
                << (targetFrequencyHz / 1000U) << " kHz\n";
    }
  }

  const bool gainChanged = pendingGain.exchange(false);
  const bool agcChanged = pendingAGC.exchange(false);
  const bool bandwidthChanged = pendingBandwidth.exchange(false);
  if (rtlConnected && (gainChanged || agcChanged)) {
    applyRtlGainAndAgc((gainChanged && agcChanged)
                           ? "runtime/update(A+G)"
                           : (agcChanged ? "runtime/update(A)"
                                         : "runtime/update(G)"));
  }

  if (bandwidthChanged) {
    const int targetBandwidthHz = requestedBandwidthHz.load();
    const int previousBandwidthHz = appliedBandwidthHz.load();
    if (targetBandwidthHz != previousBandwidthHz) {
      dspPipeline.setBandwidthHz(targetBandwidthHz);
      if (verboseLogging) {
        std::cout << "[BW] applied W" << targetBandwidthHz << " (previous W"
                  << previousBandwidthHz << ")\n";
      }
      appliedBandwidthHz = targetBandwidthHz;
    }
  }

  // SDRplay wide-bandwidth scan: when a sweep is active, drop the hardware
  // decimation so each retune covers ~8x more spectrum (a full-band sweep is
  // otherwise dominated by SDRplay's per-retune LO settling). Switch back to
  // the 256 kHz audio rate the moment the scan ends. No-op for RTL (the lambda
  // returns 0, so the normal rate is kept).
  const bool scanActive = scanEngine.isActive();
  if (scanActive && !scanWideActive) {
    const uint32_t wide = setScanWideMode(true);
    scanWideRate = (wide != 0) ? wide : iqSampleRate;
    scanWideActive = true;
  } else if (!scanActive && scanWideActive) {
    setScanWideMode(false);
    scanWideActive = false;
    scanWideRate = iqSampleRate;
  }
  const uint32_t effectiveScanRate = scanWideActive ? scanWideRate : iqSampleRate;

  return scanEngine.runIfActive(
      xdrServer, rtlConnected, shouldRun, tunerSetFrequency, tunerReadIQ,
      writeIqCapture, scanRetrySleep, iqBuffer, sdrBufSamples, effectiveScanRate,
      effectiveAppliedGainDb, signalGainCompFactor, sdrConfig, restoreAfterScan,
      tunerFlush);
}

void maybeAdjustAutoGain(
    bool useSdrppGainStrategy, int cliGain, bool imsAgcEnabled,
    std::atomic<int> &requestedAGCMode, std::atomic<bool> &pendingAGC,
    std::chrono::steady_clock::time_point &lastGainDown,
    std::chrono::steady_clock::time_point &lastGainUp,
    const SignalLevelResult &signal, double clipRatio, float rfLevelFiltered,
    bool verboseLogging, const std::function<int(int)> &agcModeToGainDb,
    const char *fixedGainAdvice) {
  if (useSdrppGainStrategy || cliGain >= 0 || imsAgcEnabled) {
    // The ladder cannot act on these paths: the gain is pinned by the
    // strategy, by the command line, or by the tuner AGC. When the front end
    // is clipping, say so and name the knob, because a clipped front end
    // mixes a strong station with itself and the IMD product is heard as a
    // duplicate of that station.
    if (fixedGainAdvice != nullptr &&
        (clipRatio > 0.0200 || signal.dbfs > -5.0)) {
      static uint32_t fixedGainClipWarnCount = 0;
      const uint32_t warnCount = ++fixedGainClipWarnCount;
      if (warnCount <= 3 || (warnCount % 50) == 0) {
        std::cerr << "[GAIN] clip-protect inactive (" << fixedGainAdvice
                  << "): clip=" << std::fixed << std::setprecision(4)
                  << clipRatio << " dbfs=" << std::setprecision(2)
                  << signal.dbfs
                  << ". The front end is clipping, so a strong station is mixed"
                     " with itself and heard as a duplicate. Lower the gain.\n";
      }
    }
    return;
  }
  // Recovery keys off raw-dbfs headroom (see auto_gain_policy.h), not the
  // smoothed display level whose slow fall used to delay climbing back.
  (void)rfLevelFiltered;

  const auto now = std::chrono::steady_clock::now();
  const bool downTimerElapsed =
      (now - lastGainDown) >= std::chrono::milliseconds(900);
  const bool upTimerElapsed =
      (now - lastGainUp) >= std::chrono::milliseconds(4000);
  const int current = std::clamp(requestedAGCMode.load(), 0, 3);
  const fm_tuner::AutoGainStep step = fm_tuner::decideAutoGainStep(
      current, clipRatio, signal.dbfs, downTimerElapsed, upTimerElapsed);

  if (step == fm_tuner::AutoGainStep::Down) {
    requestedAGCMode = current + 1;
    pendingAGC = true;
    lastGainDown = now;
    if (verboseLogging) {
      std::cout << "[GAIN] clip-protect: A" << current << " -> A"
                << (current + 1) << " (" << agcModeToGainDb(current)
                << " dB -> " << agcModeToGainDb(current + 1)
                << " dB, dbfs=" << std::fixed << std::setprecision(2)
                << signal.dbfs << ", clip=" << std::setprecision(4) << clipRatio
                << ")\n";
    }
  } else if (step == fm_tuner::AutoGainStep::Up) {
    requestedAGCMode = current - 1;
    pendingAGC = true;
    lastGainUp = now;
    if (verboseLogging) {
      std::cout << "[GAIN] sensitivity-up: A" << current << " -> A"
                << (current - 1) << " (" << agcModeToGainDb(current)
                << " dB -> " << agcModeToGainDb(current - 1)
                << " dB, dbfs=" << std::fixed << std::setprecision(2)
                << signal.dbfs << ", clip=" << std::setprecision(4) << clipRatio
                << ")\n";
    }
  }
}

void maybeAdjustAdaptiveBandwidth(
    fm_tuner::AdaptiveBandwidthMode mode,
    fm_tuner::AdaptiveBandwidthState &state,
    std::atomic<int> &requestedBandwidthHz, std::atomic<bool> &pendingBandwidth,
    int currentAppliedBandwidthHz, const SignalLevelResult &signal,
    bool verboseLogging) {
  if (mode == fm_tuner::AdaptiveBandwidthMode::Off) {
    return;
  }
  const int proposed = fm_tuner::pickAdaptiveBandwidthHz(mode, signal.snrDb);
  if (proposed <= 0) {
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  const int change = fm_tuner::applyAdaptiveBandwidthHysteresis(
      state, proposed, currentAppliedBandwidthHz, now);
  if (change <= 0) {
    return;
  }
  requestedBandwidthHz = change;
  pendingBandwidth = true;
  if (verboseLogging) {
    std::cout << "[BW] adaptive: snr=" << std::fixed << std::setprecision(1)
              << signal.snrDb << " dB, requesting " << change << " Hz (from "
              << currentAppliedBandwidthHz << ")\n";
  }
}

} // namespace runtime_loop
