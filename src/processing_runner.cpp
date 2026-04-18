#include "processing_runner.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>

namespace processing_runner {

namespace {

bool suspectGhostingFromOverload(const SignalLevelResult &signal,
                                 double clipRatio) {
  return clipRatio >= 0.01 && signal.level120 >= 70.0f && signal.dbfs >= -35.0;
}

std::string formatMaybeDouble(double value, int precision) {
  if (!std::isfinite(value)) {
    return "n/a";
  }
  std::ostringstream oss;
  oss << std::fixed << std::setprecision(precision) << value;
  return oss.str();
}

} // namespace

bool processAudioBlock(
    const uint8_t *iqBuffer, size_t samples, int outputRate,
    uint32_t iqSampleRate, int channelBandwidthHz,
    int effectiveAppliedGainDb, double signalGainCompFactor,
    const Config &config, bool verboseLogging,
    SignalLevelSmoother &rfLevelSmoother,
    const std::function<void(const SignalLevelResult &, double, float)>
        &autoGainHook,
    bool targetForceMono, bool &appliedEffectiveForceMono, DspPipeline &dspPipeline,
    RdsWorker &rdsWorker, XDRServer &xdrServer,
    size_t &retuneMuteSamplesRemaining, size_t &retuneMuteTotalSamples,
    AudioOutput &audioOut, WavWriter *mpxWavOut) {
  SignalLevelResult signal = computeSignalLevel(
      iqBuffer, samples, effectiveAppliedGainDb, signalGainCompFactor,
      config.sdr.signal_bias_db, config.sdr.signal_floor_dbfs,
      config.sdr.signal_ceil_dbfs, iqSampleRate, channelBandwidthHz);
  SignalLevelResult displaySignal = signal;

  const bool effectiveForceMono = targetForceMono;
  if (effectiveForceMono != appliedEffectiveForceMono) {
    dspPipeline.setForceMono(effectiveForceMono);
    appliedEffectiveForceMono = effectiveForceMono;
  }

  DspPipeline::Result dspOut;
  const bool haveDsp = dspPipeline.process(
      iqBuffer, samples,
      [&](const float *mpx, size_t count) {
        rdsWorker.enqueue(mpx, count);
        if (mpxWavOut != nullptr) {
          (void)mpxWavOut->enqueueMonoFloat(mpx, count);
        }
      },
      dspOut);
  if (!haveDsp) {
    return false;
  }

  if (std::isfinite(dspOut.channelPowerDbfs)) {
    displaySignal.dbfs = dspOut.channelPowerDbfs;
    displaySignal.compensatedDbfs =
        displaySignal.dbfs - (static_cast<double>(effectiveAppliedGainDb) *
                              signalGainCompFactor) +
        config.sdr.signal_bias_db;
    displaySignal.noiseFloorDbfs = std::numeric_limits<double>::quiet_NaN();
    displaySignal.snrDb = std::numeric_limits<double>::quiet_NaN();
    displaySignal.level120 = computeDisplaySignalLevel120(
        displaySignal.dbfs, displaySignal.noiseFloorDbfs, effectiveAppliedGainDb,
        signalGainCompFactor, config.sdr.signal_bias_db,
        config.sdr.signal_floor_dbfs, config.sdr.signal_ceil_dbfs, false);
  }

  const bool stereoDetected = dspOut.stereoDetected;
  const int pilotTenthsKHz = dspOut.pilotTenthsKHz;
  const float stereoBlend = dspOut.stereoBlend;
  const float stereoQuality = dspOut.stereoQuality;
  const double clipRatio = std::max(signal.hardClipRatio, signal.nearClipRatio);
  const float rfLevelFiltered =
      smoothSignalLevel(displaySignal.level120, rfLevelSmoother);
  if (verboseLogging) {
    static uint32_t signalLogCount = 0;
    const uint32_t count = ++signalLogCount;
    if (count <= 5 || (count % 100) == 0) {
      std::cout << "[SIG] dbfs="
                << formatMaybeDouble(displaySignal.dbfs, 2)
                << " compensated="
                << formatMaybeDouble(displaySignal.compensatedDbfs, 2)
                << " floor="
                << formatMaybeDouble(displaySignal.noiseFloorDbfs, 2)
                << " snr=" << formatMaybeDouble(displaySignal.snrDb, 2)
                << " level=" << std::fixed << std::setprecision(1)
                << displaySignal.level120
                << " filtered=" << rfLevelFiltered
                << " clip=" << std::setprecision(4) << clipRatio << "\n";
      std::cout << "[ST] pilot=" << pilotTenthsKHz
                << " stereo=" << (stereoDetected ? 1 : 0)
                << " quality=" << std::setprecision(3) << stereoQuality
                << " blend=" << stereoBlend << "\n";
    }

    static bool overloadGhostHintActive = false;
    static uint32_t overloadGhostHintCount = 0;
    const bool overloadGhostSuspected =
        suspectGhostingFromOverload(displaySignal, clipRatio);
    if (overloadGhostSuspected) {
      const uint32_t count = ++overloadGhostHintCount;
      if (!overloadGhostHintActive || count <= 3 || (count % 50) == 0) {
        std::cerr << "[RF] overload/image ghosting suspected: clip="
                  << std::fixed << std::setprecision(4) << clipRatio
                  << " level=" << std::setprecision(1) << displaySignal.level120
                  << " dbfs=" << std::setprecision(2) << displaySignal.dbfs
                  << ". Lower gain first; if all stations remain shifted equally,"
                     " then check freq_correction_ppm.\n";
      }
    }
    overloadGhostHintActive = overloadGhostSuspected;
  }

  autoGainHook(signal, clipRatio, rfLevelFiltered);

  const size_t outSamples = dspOut.outSamples;
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
