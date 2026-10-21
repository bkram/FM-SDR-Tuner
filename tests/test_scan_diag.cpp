#include "catch_compat.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#define private public
#include "xdr_server.h"
#undef private

#include "scan_engine.h"

#include "signal_level.h"

namespace {

// A synthetic band: one carrier on a known absolute frequency plus noise.
// The fake tuner only emits the carrier when it lies inside the captured
// band, exactly like a real front end.
class FakeBand {
public:
  FakeBand(uint32_t sampleRateHz, uint32_t carrierHz, size_t samples,
           float amplitude, float noise)
      : m_sampleRateHz(sampleRateHz), m_carrierHz(carrierHz),
        m_samples(samples), m_amplitude(amplitude), m_noise(noise),
        m_currentHz(0), m_rng(12345) {}

  void setTune(uint32_t hz) { m_currentHz = hz; }

  size_t read(uint8_t *dst) {
    const int64_t offset = static_cast<int64_t>(m_carrierHz) -
                            static_cast<int64_t>(m_currentHz);
    const int64_t fs = static_cast<int64_t>(m_sampleRateHz);
    const bool inBand = (offset > -fs / 2) && (offset < fs / 2);
    std::uniform_int_distribution<int> dist(
        -static_cast<int>(m_noise * 127.5f),
        static_cast<int>(m_noise * 127.5f));
    constexpr float kPi = 3.14159265358979323846f;
    for (size_t i = 0; i < m_samples; i++) {
      float iSample = dist(m_rng) / 127.5f;
      float qSample = dist(m_rng) / 127.5f;
      if (inBand) {
        const float phase = 2.0f * kPi * static_cast<float>(offset) *
                             (static_cast<float>(i) /
                              static_cast<float>(m_sampleRateHz));
        iSample += std::cos(phase) * m_amplitude;
        qSample += std::sin(phase) * m_amplitude;
      }
      dst[i * 2] = static_cast<uint8_t>(
          std::clamp(std::lround(iSample * 127.5f + 127.5f), 0L, 255L));
      dst[i * 2 + 1] = static_cast<uint8_t>(
          std::clamp(std::lround(qSample * 127.5f + 127.5f), 0L, 255L));
    }
    return m_samples;
  }

private:
  uint32_t m_sampleRateHz;
  uint32_t m_carrierHz;
  size_t m_samples;
  float m_amplitude;
  float m_noise;
  uint32_t m_currentHz;
  std::mt19937 m_rng;
};

std::map<int, float> parseScanLine(const std::string &line) {
  std::map<int, float> values;
  std::stringstream ss(line.substr(1));
  std::string item;
  while (std::getline(ss, item, ',')) {
    if (item.empty()) {
      continue;
    }
    const size_t eq = item.find('=');
    if (eq == std::string::npos) {
      continue;
    }
    values[std::stoi(item.substr(0, eq))] = std::stof(item.substr(eq + 1));
  }
  return values;
}

struct ScanOutcome {
  bool ran = false;
  std::map<int, float> levels;
};

// Runs a full sweep over the band and returns the published grid.
ScanOutcome runScan(uint32_t carrierHz, int stepKHz, int startKHz = 87500,
                    int stopKHz = 108000, int channelBwHz = 56000) {
  constexpr uint32_t kSampleRateHz = 256000;
  constexpr size_t kSamples = 16384;

  XDRServer xdr;
  xdr.setVerboseLogging(false);
  xdr.m_scanStartKHz = startKHz;
  xdr.m_scanStopKHz = stopKHz;
  xdr.m_scanStepKHz = stepKHz;
  xdr.m_scanBandwidthHz = channelBwHz;
  xdr.m_scanAntenna = 0;
  xdr.m_scanContinuous = false;
  xdr.m_scanStartPending = true;

  ScanEngine scan;
  std::atomic<int> requestedBandwidthHz{0};
  std::atomic<bool> pendingBandwidth{false};
  scan.handleControl(xdr, 96200000U, channelBwHz, true, false,
                     requestedBandwidthHz, pendingBandwidth,
                     [](uint32_t, int) {});

  Config::SDRSection sdrConfig{};
  FakeBand band(kSampleRateHz, carrierHz, kSamples,
                carrierHz == 0 ? 0.0f : 0.5f, 0.05f);
  std::vector<uint8_t> buffer(kSamples * 2, 127);

  ScanOutcome outcome;
  outcome.ran = scan.runIfActive(
      xdr, true, []() { return true; },
      [&](uint32_t hz) -> bool {
        band.setTune(hz);
        return true;
      },
      [&](uint8_t *dst, size_t) -> size_t { return band.read(dst); },
      [](const uint8_t *, size_t) {}, std::chrono::milliseconds(0), buffer.data(),
      kSamples, kSampleRateHz, 0, 0.0, sdrConfig, [](uint32_t, int) {});

  std::lock_guard<std::mutex> lock(xdr.m_scanMutex);
  if (!xdr.m_scanQueue.empty()) {
    outcome.levels = parseScanLine(xdr.m_scanQueue.back().second);
  }
  return outcome;
}

} // namespace

TEST_CASE("ScanEngine reports a station on the grid point of its own frequency",
          "[scan_engine][xdr]") {
  // A station on 96.2 MHz must be reported at 96.2 — not at 96.1 (the grid
  // point half a raster step away) and not dropped entirely.
  constexpr uint32_t kCarrierHz = 96200000;

  SECTION("100 kHz step") {
    const ScanOutcome outcome = runScan(kCarrierHz, 100);
    REQUIRE(outcome.ran);
    REQUIRE(outcome.levels.count(96200) == 1);
    REQUIRE(outcome.levels.at(96200) > 60.0f);
    // The neighbouring grid points must stay at the noise floor.
    REQUIRE(outcome.levels.at(96100) < 60.0f);
    REQUIRE(outcome.levels.at(96300) < 60.0f);
  }

  SECTION("200 kHz step") {
    const ScanOutcome outcome = runScan(kCarrierHz, 200);
    REQUIRE(outcome.ran);
    REQUIRE(outcome.levels.count(96200) == 1);
    REQUIRE(outcome.levels.at(96200) > 60.0f);
  }
}

TEST_CASE("ScanEngine keeps the noise floor low and flat across the band",
          "[scan_engine][xdr]") {
  // With no station the whole band must read as a flat noise floor, not as
  // a curve that rises and falls with the position of the grid point
  // inside its capture.
  const ScanOutcome outcome = runScan(0, 100);
  REQUIRE(outcome.ran);
  REQUIRE(outcome.levels.size() > 100);

  float minimum = 120.0f;
  float maximum = 0.0f;
  for (const auto &kv : outcome.levels) {
    minimum = std::min(minimum, kv.second);
    maximum = std::max(maximum, kv.second);
  }
  // The spread across the band must be a fraction of a dB-scale unit, not
  // the tens of units a positional artifact produces.
  REQUIRE((maximum - minimum) < 5.0f);
}

TEST_CASE("ScanEngine reports a station on the same scale as the signal meter",
          "[scan_engine][xdr]") {
  // The client writes the signal meter's value straight into the scan plot
  // (research/xdr-gtk/src/ui-tuner-update.c:
  //   scan_update_value(tuner_get_freq(), tuner.signal)), so the scan and
  // the meter have to be on the same scale. Otherwise the tuned station
  // jumps when it is added to the plot, and the plot and the meter
  // disagree about how strong a station is.
  //
  // Both integrate the same channel bandwidth, so they must agree closely.
  constexpr uint32_t kCarrierHz = 96200000;
  constexpr uint32_t kSampleRateHz = 256000;
  constexpr size_t kSamples = 16384;
  constexpr int kChannelBwHz = 56000;

  const ScanOutcome outcome = runScan(kCarrierHz, 100, 87500, 108000, kChannelBwHz);
  REQUIRE(outcome.ran);
  REQUIRE(outcome.levels.count(96200) == 1);
  const float scanLevel = outcome.levels.at(96200);

  // The meter reading of the same station, captured with the tuner parked
  // 10 kHz off the carrier — inside the channel window but clear of the
  // DC-reject notch, which is where both estimators can see the carrier.
  FakeBand band(kSampleRateHz, kCarrierHz, kSamples, 0.5f, 0.05f);
  band.setTune(kCarrierHz - 10000);
  std::vector<uint8_t> buffer(kSamples * 2, 127);
  band.read(buffer.data());

  Config::SDRSection sdrConfig{};
  const SignalLevelResult meter = computeSignalLevel(
      buffer.data(), kSamples, 0, 0.0, sdrConfig.signal_bias_db,
      sdrConfig.signal_floor_dbfs, sdrConfig.signal_ceil_dbfs, kSampleRateHz,
      kChannelBwHz);

  // The scan integrates the full channel bandwidth, so the two agree to
  // within a fraction of a dB-scale unit (2 units per dB).
  REQUIRE(std::abs(scanLevel - meter.level120) < 6.0f);
}

TEST_CASE("ScanEngine aligns the reporting grid with the station raster",
          "[scan_engine][xdr]") {
  // The European FM raster is 87.6 + n * 0.2 MHz. A grid whose step
  // divides that raster must put every station on its own frequency.
  //
  // A 100 kHz step from 87.5 MHz already contains 87.6, 87.8, ... , so the
  // requested start is kept as-is.
  {
    const ScanOutcome outcome = runScan(0, 100);
    REQUIRE(outcome.ran);
    REQUIRE(outcome.levels.count(87500) == 1);
    REQUIRE(outcome.levels.count(87600) == 1);
    // Every station frequency must be a grid point of its own.
    for (int stationKHz = 87600; stationKHz <= 108000; stationKHz += 200) {
      REQUIRE(outcome.levels.count(stationKHz) == 1);
    }
  }

  // A 200 kHz step cannot hold every station from a 87.5 MHz anchor: the
  // grid would land on the midpoints between the stations, so a station on
  // 96.2 MHz would be reported on 96.1. The grid is shifted onto the raster.
  {
    const ScanOutcome outcome = runScan(0, 200);
    REQUIRE(outcome.ran);
    REQUIRE(outcome.levels.count(87500) == 0);
    REQUIRE(outcome.levels.count(87600) == 1);
    REQUIRE(outcome.levels.count(96200) == 1);
    REQUIRE(outcome.levels.count(96100) == 0);
  }
}
