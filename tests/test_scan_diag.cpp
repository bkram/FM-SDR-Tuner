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

#include "fake_band.h"

namespace {

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
                    int stopKHz = 108000, int channelBwHz = 56000,
                    float amplitude = 0.3f, float noise = 0.05f,
                    bool useClientWindow = false) {
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

  Config::SDRSection sdrConfig = Config::SDRSection{};
  sdrConfig.scan_use_client_window = useClientWindow;
  FakeBand band(kSampleRateHz, carrierHz, kSamples, amplitude, noise);
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
    // The neighbouring grid points carry the spectrum around them, not a
    // signal of their own: they stay well below the station.
    REQUIRE(outcome.levels.at(96100) < outcome.levels.at(96200) * 0.7f);
    REQUIRE(outcome.levels.at(96300) < outcome.levels.at(96200) * 0.7f);
  }

  SECTION("200 kHz step") {
    const ScanOutcome outcome = runScan(kCarrierHz, 200);
    REQUIRE(outcome.ran);
    REQUIRE(outcome.levels.count(96200) == 1);
    REQUIRE(outcome.levels.at(96200) > 60.0f);
    REQUIRE(outcome.levels.count(96100) == 0);
  }
}

TEST_CASE("ScanEngine reports a station that sits between two grid points on the nearest one",
          "[scan_engine][xdr]") {
  // A station that is not exactly on a grid point belongs to the grid point
  // nearest to it. 96.1765 MHz is 23.5 kHz from 96.2 and 76.5 kHz from 96.1,
  // so it has to be reported on 96.2.
  constexpr uint32_t kCarrierHz = 96176500;

  const ScanOutcome outcome = runScan(kCarrierHz, 100);
  REQUIRE(outcome.ran);
  REQUIRE(outcome.levels.count(96200) == 1);
  REQUIRE(outcome.levels.at(96200) > 60.0f);
  // The grid points around the station carry the spectrum around them, not a
  // signal of their own.
  REQUIRE(outcome.levels.at(96100) < outcome.levels.at(96200) * 0.7f);
  REQUIRE(outcome.levels.at(96300) < outcome.levels.at(96200) * 0.7f);

  // The station's own grid point is the strongest point of the plot.
  for (const auto &kv : outcome.levels) {
    REQUIRE(kv.second <= outcome.levels.at(96200));
  }
}

TEST_CASE("ScanEngine keeps the noise floor low and flat across the band",
          "[scan_engine][xdr]") {
  // With no station the whole band must read as the same flat floor, not as
  // a curve that rises and falls with the position of the grid point inside
  // its capture.
  //
  // The floor is the spectrum of an empty band: the client draws it as a
  // line at the bottom of the plot. That is what keeps a signal that is not
  // a station a bump in the floor instead of a spike of its own - the
  // difference between a spectrum display and a row of ghosts.
  const ScanOutcome outcome = runScan(0, 100);
  REQUIRE(outcome.ran);
  REQUIRE(outcome.levels.size() > 100);

  float minLevel = 120.0f;
  float maxLevel = 0.0f;
  for (const auto &kv : outcome.levels) {
    minLevel = std::min(minLevel, kv.second);
    maxLevel = std::max(maxLevel, kv.second);
  }
  INFO("floor min=" << minLevel << " max=" << maxLevel);
  REQUIRE(maxLevel - minLevel < 10.0f);

  // The floor of an empty band sits far below the station a sweep of the
  // same band finds.
  const ScanOutcome withStation = runScan(96200000, 100);
  REQUIRE(withStation.ran);
  REQUIRE(withStation.levels.at(96200) > maxLevel * 1.4f);
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
  // Both integrate the same window around the same point, so they have to
  // report the very same number.
  constexpr uint32_t kCarrierHz = 96200000;
  constexpr uint32_t kSampleRateHz = 256000;
  constexpr size_t kSamples = 16384;
  constexpr float kAmplitude = 0.05f;
  constexpr float kNoise = 0.01f;

  const ScanOutcome outcome =
      runScan(kCarrierHz, 100, 87500, 108000, 56000, kAmplitude, kNoise);
  REQUIRE(outcome.ran);
  REQUIRE(outcome.levels.count(96200) == 1);
  const float scanLevel = outcome.levels.at(96200);
  REQUIRE(scanLevel > 0.0f);

  // The meter reading of the same station, captured with the tuner parked
  // on the carrier.
  FakeBand band(kSampleRateHz, kCarrierHz, kSamples, kAmplitude, kNoise);
  band.setTune(kCarrierHz);
  std::vector<uint8_t> buffer(kSamples * 2, 127);
  band.read(buffer.data());

  Config::SDRSection sdrConfig = Config::SDRSection{};
  const SignalLevelResult meter = computeSignalLevel(
      buffer.data(), kSamples, 0, 0.0, sdrConfig.signal_bias_db,
      sdrConfig.signal_floor_dbfs, sdrConfig.signal_ceil_dbfs, kSampleRateHz);

  INFO("scan=" << scanLevel << " meter=" << meter.level120
               << " meter_dbfs=" << meter.dbfs << " meter_noise=" << meter.noiseFloorDbfs);
  REQUIRE(std::abs(scanLevel - meter.level120) < 2.0f);
}

TEST_CASE("ScanEngine reports levels on the meter's own scale", "[scan_engine][xdr]") {
  // The scan reports the meter's 0..120 scale, so the plot and the meter
  // can be read against each other. Every grid point carries the level of
  // the spectrum at that point, so the plot reads as a spectrum and not as a
  // list of stations.
  const ScanOutcome outcome = runScan(96200000, 100);
  REQUIRE(outcome.ran);
  REQUIRE(outcome.levels.size() > 100);

  for (const auto &kv : outcome.levels) {
    REQUIRE(kv.second >= 0.0f);
    REQUIRE(kv.second <= 120.0f);
  }
}

TEST_CASE("ScanEngine can remap the levels onto the client's plot window",
          "[scan_engine][xdr]") {
  // The reference client draws the scan in a fixed 3..84 window and clips
  // everything outside it, so a level above 84 is pinned flat against the
  // top of the plot and strong stations stop telling each other apart.
  // With the remap on, every reported level has to land inside the window.
  const ScanOutcome outcome = runScan(96200000, 100, 87500, 108000, 56000,
                                      0.3f, 0.05f, true);
  REQUIRE(outcome.ran);
  REQUIRE(outcome.levels.size() > 100);

  for (const auto &kv : outcome.levels) {
    // The empty cells keep the client's "no signal" value.
    if (kv.second == 0.0f) {
      continue;
    }
    REQUIRE(kv.second >= 3.0f);
    REQUIRE(kv.second <= 84.0f);
  }
  REQUIRE(outcome.levels.at(96200) > 3.0f);
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
