#include "processing_runner.h"

#include <algorithm>
#include <iomanip>
#include <iostream>

namespace processing_runner {

bool processAudioBlock(
    const uint8_t *iqBuffer, size_t samples, int outputRate,
    int effectiveAppliedGainDb, double signalGainCompFactor,
    const Config &config, bool verboseLogging,
    SignalLevelSmoother &rfLevelSmoother,
    const std::function<void(const SignalLevelResult &, double, float)>
        &autoGainHook,
    bool targetForceMono, bool &appliedEffectiveForceMono, DspPipeline &dspPipeline,
    RdsWorker &rdsWorker, XDRServer &xdrServer,
    size_t &retuneMuteSamplesRemaining, size_t &retuneMuteTotalSamples,
    AudioOutput &audioOut) {
  const SignalLevelResult signal = computeSignalLevel(
      iqBuffer, samples, effectiveAppliedGainDb, signalGainCompFactor,
      config.sdr.signal_bias_db, config.sdr.signal_floor_dbfs,
      config.sdr.signal_ceil_dbfs);
  const double clipRatio = std::max(signal.hardClipRatio, signal.nearClipRatio);
  const float rfLevelFiltered = smoothSignalLevel(signal.level120, rfLevelSmoother);
  if (verboseLogging) {
    static uint32_t signalLogCount = 0;
    const uint32_t count = ++signalLogCount;
    if (count <= 5 || (count % 100) == 0) {
      std::cout << "[SIG] dbfs=" << std::fixed << std::setprecision(2)
                << signal.dbfs << " compensated=" << signal.compensatedDbfs
                << " level=" << std::setprecision(1) << signal.level120
                << " filtered=" << rfLevelFiltered
                << " clip=" << std::setprecision(4) << clipRatio << "\n";
    }
  }

  autoGainHook(signal, clipRatio, rfLevelFiltered);

  const bool effectiveForceMono = targetForceMono;
  if (effectiveForceMono != appliedEffectiveForceMono) {
    dspPipeline.setForceMono(effectiveForceMono);
    appliedEffectiveForceMono = effectiveForceMono;
  }

  DspPipeline::Result dspOut;
  const bool haveDsp = dspPipeline.process(
      iqBuffer, samples,
      [&](const float *mpx, size_t count) { rdsWorker.enqueue(mpx, count); },
      dspOut);
  if (!haveDsp) {
    return false;
  }

  const size_t outSamples = dspOut.outSamples;
  const bool stereoDetected = dspOut.stereoDetected;
  const int pilotTenthsKHz = dspOut.pilotTenthsKHz;
  float *audioLeft = dspOut.left;
  float *audioRight = dspOut.right;
  const bool stereoIndicator = stereoDetected ||
                               (effectiveForceMono && config.processing.stereo &&
                                pilotTenthsKHz >= 20);
  xdrServer.updateSignal(rfLevelFiltered, stereoIndicator, effectiveForceMono, -1,
                         -1);
  xdrServer.updatePilot(pilotTenthsKHz);

  if (retuneMuteSamplesRemaining > 0 && outSamples > 0) {
    const size_t muteCount = std::min(outSamples, retuneMuteSamplesRemaining);
    const size_t alreadyMuted =
        (retuneMuteTotalSamples > retuneMuteSamplesRemaining)
            ? (retuneMuteTotalSamples - retuneMuteSamplesRemaining)
            : 0;
    const size_t fadeSamples = std::max<size_t>(
        1, std::min(static_cast<size_t>(outputRate / 200),
                    retuneMuteTotalSamples / 2)); // up to ~5 ms
    for (size_t i = 0; i < muteCount; i++) {
      const size_t idx = alreadyMuted + i;
      float gain = 0.0f;
      if (idx < fadeSamples) {
        gain = 1.0f - (static_cast<float>(idx) / static_cast<float>(fadeSamples));
      } else if (idx >= (retuneMuteTotalSamples - fadeSamples)) {
        const size_t tail = retuneMuteTotalSamples - idx;
        gain = static_cast<float>(tail) / static_cast<float>(fadeSamples);
      }
      gain = std::clamp(gain, 0.0f, 1.0f);
      audioLeft[i] *= gain;
      audioRight[i] *= gain;
    }
    retuneMuteSamplesRemaining -= muteCount;
    if (retuneMuteSamplesRemaining == 0) {
      retuneMuteTotalSamples = 0;
    }
  }

  if (outSamples > 0) {
    audioOut.write(audioLeft, audioRight, outSamples);
  }
  return true;
}

} // namespace processing_runner
