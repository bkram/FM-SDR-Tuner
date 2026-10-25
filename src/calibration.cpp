#include "calibration.h"

#include "dsp/iq_saturation.h"
#include "rtl_sdr_device.h"
#include "signal_level.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <limits>
#include <thread>
#include <utility>
#include <vector>

namespace fm_tuner::calibration {

namespace {

constexpr uint32_t kMinimumCaptureRateHz = 1024000;
constexpr size_t kCaptureSamples = 32768;

bool readIqWithRetries(RTLSDRDevice &dev, std::vector<uint8_t> &iq,
                       size_t requestedSamples, int attempts,
                       std::chrono::milliseconds sleepBetween,
                       size_t &samplesOut) {
  samplesOut = 0;
  iq.resize(requestedSamples * 2);
  for (int attempt = 0; attempt < attempts && samplesOut == 0; ++attempt) {
    samplesOut = dev.readIQ(iq.data(), requestedSamples);
    if (samplesOut == 0) {
      std::this_thread::sleep_for(sleepBetween);
    }
  }
  return samplesOut > 0;
}

bool settleAfterRetune(RTLSDRDevice &dev, std::vector<uint8_t> &iq,
                       uint32_t sampleRateHz) {
  dev.flushBuffers();
  size_t remaining = std::max<size_t>(8192, sampleRateHz / 10);
  while (remaining > 0) {
    const size_t requested = std::min(remaining, kCaptureSamples);
    size_t samples = 0;
    if (!readIqWithRetries(dev, iq, requested, 4, std::chrono::milliseconds(15),
                           samples)) {
      return false;
    }
    remaining -= std::min(remaining, samples);
  }
  return true;
}

double clipRatio(const uint8_t *iq, size_t samples) {
  if (!iq || samples == 0) {
    return 0.0;
  }
  size_t hard = 0;
  size_t near = 0;
  for (size_t i = 0; i < samples * 2; ++i) {
    const uint8_t value = iq[i];
    if (fm_tuner::dsp::isRtlSdrIqByteSaturated(value)) {
      hard++;
    }
    if (value <= 8 || value >= 247) {
      near++;
    }
  }
  const double count = static_cast<double>(samples * 2);
  return std::max(static_cast<double>(hard) / count,
                  static_cast<double>(near) / count);
}

} // namespace

int runBandSweep(const BandSweepOptions &options) {
  std::cout << "[CALIBRATE] opening RTL-SDR device " << options.deviceIndex
            << "...\n";
  RTLSDRDevice dev(options.deviceIndex);
  if (!dev.connect()) {
    std::cerr << "[CALIBRATE] failed to open RTL-SDR device "
              << options.deviceIndex << "\n";
    return 1;
  }

  const uint32_t captureRateHz =
      std::max(options.sampleRateHz, kMinimumCaptureRateHz);
  if (!dev.setSampleRate(captureRateHz)) {
    std::cerr << "[CALIBRATE] failed to set sample rate " << captureRateHz
              << "\n";
    dev.disconnect();
    return 1;
  }

  dev.setLowLatencyMode(false);
  const bool autoGain = options.gainDb < 0;
  if (autoGain) {
    if (!dev.setGainMode(false)) {
      std::cerr << "[CALIBRATE] failed to enable auto-gain mode\n";
      dev.disconnect();
      return 1;
    }
    std::cout << "[CALIBRATE] gain mode: auto\n";
  } else {
    if (!dev.setGainMode(true) ||
        !dev.setGain(static_cast<uint32_t>(options.gainDb * 10))) {
      std::cerr << "[CALIBRATE] failed to set manual gain\n";
      dev.disconnect();
      return 1;
    }
    std::cout << "[CALIBRATE] gain mode: manual "
              << options.gainDb << " dB\n";
  }

  const uint32_t startHz = options.startKHz * 1000U;
  const uint32_t endHz = options.endKHz * 1000U;
  const uint32_t gridStepHz = options.stepKHz * 1000U;
  const std::vector<uint32_t> centers = makeCaptureCenters(
      startHz, endHz, gridStepHz, captureRateHz, fm_tuner::kTunedMeterWindowHz);
  if (centers.empty()) {
    std::cerr << "[CALIBRATE] invalid sweep geometry\n";
    dev.disconnect();
    return 1;
  }

  const size_t pointCount = (endHz - startHz) / gridStepHz + 1;
  std::vector<Measurement> measurements(pointCount);
  for (size_t i = 0; i < pointCount; ++i) {
    measurements[i].freqKHz =
        (startHz + static_cast<uint32_t>(i) * gridStepHz) / 1000U;
    measurements[i].dbfs = std::numeric_limits<double>::quiet_NaN();
    measurements[i].compensatedDbfs = std::numeric_limits<double>::quiet_NaN();
  }

  std::cout << "[CALIBRATE] sweeping " << (options.startKHz / 1000.0) << " - "
            << (options.endKHz / 1000.0) << " MHz, " << options.stepKHz
            << " kHz grid, " << centers.size() << " wide captures for "
            << pointCount << " points\n";
  std::cout << "[CALIBRATE] measurement window: "
            << fm_tuner::kTunedMeterWindowHz
            << " Hz, temporary capture rate: " << captureRateHz << " Hz\n";

  const int appliedGainDb = autoGain ? 0 : options.gainDb;
  IqSpectrum spectrum;
  std::vector<uint8_t> iq(kCaptureSamples * 2);
  for (size_t capture = 0; capture < centers.size(); ++capture) {
    const uint32_t centerHz = centers[capture];
    if (!dev.setFrequency(centerHz) ||
        !settleAfterRetune(dev, iq, captureRateHz)) {
      std::cerr << "[CALIBRATE] warning: failed to settle at "
                << (centerHz / 1000U) << " kHz\n";
      continue;
    }

    size_t samples = 0;
    if (!readIqWithRetries(dev, iq, kCaptureSamples, 4,
                           std::chrono::milliseconds(15), samples) ||
        !spectrum.analyse(iq.data(), samples, captureRateHz)) {
      std::cerr << "[CALIBRATE] warning: no spectrum at " << (centerHz / 1000U)
                << " kHz\n";
      continue;
    }

    const double captureClipRatio = clipRatio(iq.data(), samples);
    const int64_t usableHalfSpan =
        static_cast<int64_t>(spectrum.halfSpanHz() * 0.85) -
        fm_tuner::kTunedMeterWindowHz / 2;
    for (size_t point = 0; point < measurements.size(); ++point) {
      const int64_t frequencyHz = static_cast<int64_t>(startHz) +
                                  static_cast<int64_t>(point) * gridStepHz;
      const int64_t offsetHz = frequencyHz - centerHz;
      if (std::llabs(offsetHz) > usableHalfSpan) {
        continue;
      }
      const double dbfs =
          spectrum.channelDbfs(offsetHz, fm_tuner::kTunedMeterWindowHz);
      if (!std::isfinite(dbfs)) {
        continue;
      }

      Measurement &slot = measurements[point];
      const bool candidateClipped =
          captureClipRatio >= fm_tuner::kCalibrationClipRatio;
      const bool slotClipped =
          slot.clipRatio >= fm_tuner::kCalibrationClipRatio;
      if (!std::isfinite(slot.dbfs) || (slotClipped && !candidateClipped) ||
          (slotClipped == candidateClipped && dbfs > slot.dbfs)) {
        slot.dbfs = dbfs;
        slot.compensatedDbfs = dbfs - static_cast<double>(appliedGainDb) *
                                          fm_tuner::kSignalGainCompFactor;
        slot.clipRatio = captureClipRatio;
      }
    }

    std::cout << "[CALIBRATE] progress: " << (capture + 1) << "/"
              << centers.size() << " captures\n";
  }
  dev.disconnect();

  measurements.erase(std::remove_if(measurements.begin(), measurements.end(),
                                    [](const Measurement &measurement) {
                                      return !std::isfinite(measurement.dbfs);
                                    }),
                     measurements.end());
  if (measurements.empty()) {
    std::cerr << "[CALIBRATE] no measurements collected\n";
    return 1;
  }

  const Recommendation recommendation = recommendWindow(measurements);
  if (!recommendation.valid) {
    std::cerr
        << "[CALIBRATE] too few clean measurements; lower gain and retry\n";
    return 1;
  }

  std::cout << "\n[CALIBRATE] per-frequency results:\n";
  std::cout << "  freq_MHz   dBFS   compensated   level120   clip\n";
  for (const Measurement &row : measurements) {
    const float level = computeDisplaySignalLevel120(
        row.dbfs, 0.0, appliedGainDb, fm_tuner::kSignalGainCompFactor, 0.0,
        recommendation.floorDbfs, recommendation.ceilDbfs, false);
    std::printf("  %7.1f   %5.1f      %5.1f       %6.1f   %.4f%s\n",
                row.freqKHz / 1000.0, row.dbfs, row.compensatedDbfs, level,
                row.clipRatio,
                row.clipRatio >= fm_tuner::kCalibrationClipRatio ? " *" : "");
  }

  std::cout << "\n[CALIBRATE] detected stations (level120 > 30):\n";
  std::cout << "  freq_MHz   compensated   level120\n";
  std::vector<std::pair<float, const Measurement *>> stations;
  for (const Measurement &row : measurements) {
    if (row.clipRatio >= fm_tuner::kCalibrationClipRatio) {
      continue;
    }
    const float level = computeDisplaySignalLevel120(
        row.dbfs, 0.0, appliedGainDb, fm_tuner::kSignalGainCompFactor, 0.0,
        recommendation.floorDbfs, recommendation.ceilDbfs, false);
    if (level > 30.0f) {
      stations.emplace_back(level, &row);
    }
  }
  std::sort(stations.begin(), stations.end(),
            [](const auto &a, const auto &b) { return a.first > b.first; });
  for (const auto &[level, row] : stations) {
    std::printf("  %7.1f      %5.1f       %6.1f\n", row->freqKHz / 1000.0,
                row->compensatedDbfs, level);
  }
  if (stations.empty()) {
    std::cout << "  (none — check antenna connection and signal coverage)\n";
  }

  std::cout << "\n[CALIBRATE] clean compensated envelope: min="
            << recommendation.minimumDbfs << " p05=" << recommendation.p05Dbfs
            << " median=" << recommendation.medianDbfs
            << " p95=" << recommendation.p95Dbfs
            << " max=" << recommendation.maximumDbfs << " dBFS\n";
  if (recommendation.saturatedCount > 0) {
    std::cout << "[CALIBRATE] " << recommendation.saturatedCount
              << " grid point(s) excluded because their capture clipped. "
                 "Lower gain and recalibrate if important stations are marked "
                 "with *.\n";
  }

  std::cout << "\n[CALIBRATE] recommended INI values for this antenna / "
               "location — paste into the [sdr] section of your config:\n\n";
  std::cout << "    signal_floor_dbfs = " << recommendation.floorDbfs << "\n";
  std::cout << "    signal_ceil_dbfs  = " << recommendation.ceilDbfs << "\n";
  std::cout << "    signal_bias_db    = 0.0\n";
  std::cout << "\n    # window: "
            << (recommendation.ceilDbfs - recommendation.floorDbfs)
            << " dB; recommendation uses clean compensated measurements and "
               "ignores the existing meter window\n\n";
  return 0;
}

} // namespace fm_tuner::calibration
