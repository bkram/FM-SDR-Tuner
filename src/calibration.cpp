#include "calibration.h"

#include "rtl_sdr_device.h"
#include "signal_level.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <thread>
#include <vector>

namespace fm_tuner::calibration {

namespace {

bool readIqWithRetries(RTLSDRDevice &dev, std::vector<uint8_t> &iq,
                      size_t requestedSamples, int attempts,
                      std::chrono::milliseconds sleepBetween,
                      size_t &samplesOut) {
  samplesOut = 0;
  for (int attempt = 0; attempt < attempts && samplesOut == 0; ++attempt) {
    samplesOut = dev.readIQ(iq.data(), requestedSamples);
    if (samplesOut == 0) {
      std::this_thread::sleep_for(sleepBetween);
    }
  }
  return samplesOut > 0;
}

bool measureFrequency(RTLSDRDevice &dev, uint32_t freqKHz, uint32_t sampleRateHz,
                      int appliedGainDb, int channelBandwidthHz,
                      SignalLevelResult &out) {
  if (!dev.setFrequency(freqKHz * 1000U)) {
    return false;
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(120));

  // Burn 4 buffer-loads to flush any in-flight USB samples from the previous
  // tune; without this the FIFO holds stale data and measurements smear across
  // recent neighbour frequencies.
  std::vector<uint8_t> iq(4096 * 2, 0);
  size_t discarded = 0;
  for (int flush = 0; flush < 4; ++flush) {
    (void)readIqWithRetries(dev, iq, iq.size() / 2, 4,
                            std::chrono::milliseconds(15), discarded);
  }

  double dbfsSum = 0.0;
  double compensatedSum = 0.0;
  double levelSum = 0.0;
  int validBlocks = 0;

  for (int block = 0; block < 3; ++block) {
    size_t samples = 0;
    if (!readIqWithRetries(dev, iq, iq.size() / 2, 8,
                           std::chrono::milliseconds(15), samples)) {
      continue;
    }
    iq.resize(samples * 2);

    const SignalLevelResult signal = computeSignalLevel(
        iq.data(), samples, appliedGainDb, 0.5, 0.0, -80.0, -12.0, sampleRateHz,
        channelBandwidthHz);
    if (!std::isfinite(signal.dbfs)) {
      iq.assign(4096 * 2, 0);
      continue;
    }
    dbfsSum += signal.dbfs;
    compensatedSum += signal.compensatedDbfs;
    levelSum += signal.level120;
    validBlocks++;
    iq.assign(4096 * 2, 0);
  }

  if (validBlocks == 0) {
    return false;
  }
  out = {};
  out.dbfs = dbfsSum / validBlocks;
  out.compensatedDbfs = compensatedSum / validBlocks;
  out.level120 = static_cast<float>(levelSum / validBlocks);
  return true;
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
  if (!dev.setSampleRate(options.sampleRateHz)) {
    std::cerr << "[CALIBRATE] failed to set sample rate "
              << options.sampleRateHz << "\n";
    dev.disconnect();
    return 1;
  }

  dev.setLowLatencyMode(true);
  const bool autoGain = options.gainTenthsDb < 0;
  if (autoGain) {
    if (!dev.setGainMode(false)) {
      std::cerr << "[CALIBRATE] failed to enable auto-gain mode\n";
      dev.disconnect();
      return 1;
    }
    std::cout << "[CALIBRATE] gain mode: auto\n";
  } else {
    if (!dev.setGainMode(true) ||
        !dev.setGain(static_cast<uint32_t>(options.gainTenthsDb))) {
      std::cerr << "[CALIBRATE] failed to set manual gain\n";
      dev.disconnect();
      return 1;
    }
    std::cout << "[CALIBRATE] gain mode: manual "
              << (options.gainTenthsDb / 10.0) << " dB\n";
  }

  const uint32_t totalSteps =
      (options.endKHz - options.startKHz) / options.stepKHz + 1;
  std::cout << "[CALIBRATE] sweeping " << (options.startKHz / 1000.0) << " - "
            << (options.endKHz / 1000.0) << " MHz, step "
            << options.stepKHz << " kHz, " << totalSteps << " points\n";
  std::cout << "[CALIBRATE] channel BW: " << options.channelBandwidthHz
            << " Hz, sample rate: " << options.sampleRateHz << " Hz\n";

  struct Row {
    uint32_t freqKHz;
    double dbfs;
    double compensatedDbfs;
    double level120;
  };
  std::vector<Row> rows;
  rows.reserve(totalSteps);

  uint32_t step = 0;
  for (uint32_t f = options.startKHz; f <= options.endKHz;
       f += options.stepKHz) {
    SignalLevelResult r;
    if (!measureFrequency(dev, f, options.sampleRateHz,
                          autoGain ? 0 : options.gainTenthsDb / 10,
                          options.channelBandwidthHz, r)) {
      continue;
    }
    rows.push_back({f, r.dbfs, r.compensatedDbfs, r.level120});
    step++;
    if (step % 20 == 0) {
      std::cout << "[CALIBRATE] progress: " << step << "/" << totalSteps << "\n";
    }
  }
  dev.disconnect();

  if (rows.empty()) {
    std::cerr << "[CALIBRATE] no measurements collected\n";
    return 1;
  }

  std::cout << "\n[CALIBRATE] per-frequency results:\n";
  std::cout << "  freq_MHz   dBFS   level120\n";
  for (const Row &row : rows) {
    std::printf("  %7.1f   %5.1f   %6.1f\n", row.freqKHz / 1000.0, row.dbfs,
                row.level120);
  }

  std::cout << "\n[CALIBRATE] detected stations (level120 > 30):\n";
  std::cout << "  freq_MHz   dBFS   level120\n";
  std::vector<Row> stations;
  for (const Row &row : rows) {
    if (row.level120 > 30.0) {
      stations.push_back(row);
    }
  }
  std::sort(stations.begin(), stations.end(),
            [](const Row &a, const Row &b) { return a.level120 > b.level120; });
  for (const Row &row : stations) {
    std::printf("  %7.1f   %5.1f   %6.1f\n", row.freqKHz / 1000.0, row.dbfs,
                row.level120);
  }
  if (stations.empty()) {
    std::cout << "  (none — check antenna connection and signal coverage)\n";
  }

  std::vector<double> dbfsSorted;
  dbfsSorted.reserve(rows.size());
  for (const Row &row : rows) {
    dbfsSorted.push_back(row.dbfs);
  }
  std::sort(dbfsSorted.begin(), dbfsSorted.end());
  const double p05 = dbfsSorted[static_cast<size_t>(dbfsSorted.size() * 0.05)];
  const double p50 = dbfsSorted[dbfsSorted.size() / 2];
  const double p95 = dbfsSorted[static_cast<size_t>(dbfsSorted.size() * 0.95)];
  const double pMax = dbfsSorted.back();
  const double pMin = dbfsSorted.front();

  std::cout << "\n[CALIBRATE] signal envelope: min=" << pMin << " p05=" << p05
            << " median=" << p50 << " p95=" << p95 << " max=" << pMax
            << " dBFS\n";

  const int recommendedFloor = static_cast<int>(std::round(p05 - 3.0));
  const int recommendedCeil = static_cast<int>(std::round(p95 + 1.0));
  std::cout << "\n[CALIBRATE] recommended INI values for this antenna / "
               "location — paste into the [sdr] section of your config:\n\n";
  std::cout << "    signal_floor_dbfs = " << recommendedFloor << "\n";
  std::cout << "    signal_ceil_dbfs  = " << recommendedCeil << "\n";
  std::cout << "    signal_bias_db    = 0.0\n";
  std::cout << "\n    # window: " << (recommendedCeil - recommendedFloor)
            << " dB (default is 60 dB; tighter = finer per-step meter "
               "resolution at this location)\n\n";
  return 0;
}

} // namespace fm_tuner::calibration
