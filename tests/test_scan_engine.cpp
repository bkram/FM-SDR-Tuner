#include "catch_compat.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#define private public
#include "xdr_server.h"
#undef private

#include "scan_engine.h"

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

} // namespace

TEST_CASE("ScanEngine emits xdr-gtk-compatible U lines with trailing comma",
          "[scan_engine][xdr]") {
  XDRServer xdr;
  xdr.setVerboseLogging(false);

  xdr.m_scanStartKHz = 87500;
  xdr.m_scanStopKHz = 87600;
  xdr.m_scanStepKHz = 100;
  xdr.m_scanBandwidthHz = 0;
  xdr.m_scanAntenna = 0;
  xdr.m_scanContinuous = false;
  xdr.m_scanStartPending = true;

  ScanEngine scan;
  std::atomic<int> requestedBandwidthHz{0};
  std::atomic<bool> pendingBandwidth{false};

  scan.handleControl(xdr, 90000000U, 56000, true, false, requestedBandwidthHz,
                     pendingBandwidth,
                     [](uint32_t, int) {});

  std::vector<uint8_t> iqBuffer(4096 * 2, 127);

  Config::SDRSection sdrConfig = Config::SDRSection{};
  const bool ran = scan.runIfActive(
      xdr, true, []() { return true; },
      [](uint32_t) -> bool { return true; },
      [](uint8_t *, size_t) -> size_t { return 0; },
      [](const uint8_t *, size_t) {}, std::chrono::milliseconds(0),
      iqBuffer.data(), 4096, 256000, 0, 0.0, sdrConfig,
      [](uint32_t, int) {});

  REQUIRE(ran);

  std::lock_guard<std::mutex> lock(xdr.m_scanMutex);
  // The scan publishes one line per retune so the client's plot fills in
  // during the sweep; the last line carries the complete grid.
  REQUIRE(!xdr.m_scanQueue.empty());
  const std::string &line = xdr.m_scanQueue.back().second;
  // Both channels were measured and neither of them holds a station, so
  // they carry the "no signal" value.
  REQUIRE(line == "U87500=0.0,87600=0.0,");
}

TEST_CASE("ScanEngine suppresses empty-channel FFT peaks with local floor gating",
          "[scan_engine][xdr]") {
  XDRServer xdr;
  xdr.setVerboseLogging(false);

  xdr.m_scanStartKHz = 87500;
  xdr.m_scanStopKHz = 87700;
  xdr.m_scanStepKHz = 100;
  // 40 kHz channel BW leaves enough room that usableHalfSpanHz is not clipped
  // by the Nyquist constraint.
  xdr.m_scanBandwidthHz = 40000;
  xdr.m_scanAntenna = 0;
  xdr.m_scanContinuous = false;
  xdr.m_scanStartPending = true;

  ScanEngine scan;
  std::atomic<int> requestedBandwidthHz{0};
  std::atomic<bool> pendingBandwidth{false};
  scan.handleControl(xdr, 90000000U, 56000, true, false, requestedBandwidthHz,
                     pendingBandwidth, [](uint32_t, int) {});

  constexpr uint32_t kSampleRateHz = 256000;
  constexpr size_t kSamples = 16384;
  // The station sits on 87.6 MHz. The fake band only emits the carrier when
  // the tuner is close enough for it to fall inside the captured band, so the
  // test does not depend on where the engine places its capture centres.
  FakeBand band(kSampleRateHz, 87600000, kSamples, 0.70f, 0.02f);
  std::vector<uint8_t> iqBuffer(kSamples * 2, 127);

  Config::SDRSection sdrConfig = Config::SDRSection{};
  const bool ran = scan.runIfActive(
      xdr, true, []() { return true; },
      [&](uint32_t hz) -> bool {
        band.setTune(hz);
        return true;
      },
      [&](uint8_t *dest, size_t maxSamples) -> size_t {
        const size_t copySamples = std::min(maxSamples, kSamples);
        if (copySamples == 0) {
          return 0;
        }
        return band.read(dest);
      },
      [](const uint8_t *, size_t) {}, std::chrono::milliseconds(0),
      iqBuffer.data(), kSamples, kSampleRateHz, 0, 0.0, sdrConfig,
      [](uint32_t, int) {});

  REQUIRE(ran);

  std::lock_guard<std::mutex> lock(xdr.m_scanMutex);
  REQUIRE(!xdr.m_scanQueue.empty());
  const std::map<int, float> values = parseScanLine(xdr.m_scanQueue.back().second);
  REQUIRE(values.count(87500) == 1);
  REQUIRE(values.count(87600) == 1);
  REQUIRE(values.count(87700) == 1);
  REQUIRE(values.at(87600) > values.at(87500));
  REQUIRE(values.at(87600) > values.at(87700));
}

TEST_CASE("ScanEngine batches fallback retunes for contiguous uncovered channels",
          "[scan_engine][xdr]") {
  XDRServer xdr;
  xdr.setVerboseLogging(false);

  xdr.m_scanStartKHz = 87500;
  xdr.m_scanStopKHz = 88000;
  xdr.m_scanStepKHz = 100;
  xdr.m_scanBandwidthHz = 56000;
  xdr.m_scanAntenna = 0;
  xdr.m_scanContinuous = false;
  xdr.m_scanStartPending = true;

  ScanEngine scan;
  std::atomic<int> requestedBandwidthHz{0};
  std::atomic<bool> pendingBandwidth{false};
  scan.handleControl(xdr, 90000000U, 56000, true, false, requestedBandwidthHz,
                     pendingBandwidth, [](uint32_t, int) {});

  constexpr uint32_t kSampleRateHz = 256000;
  constexpr size_t kSamples = 16384;
  std::vector<uint8_t> iqBuffer(kSamples * 2, 127);

  int retuneCount = 0;
  int readCount = 0;
  Config::SDRSection sdrConfig = Config::SDRSection{};
  const bool ran = scan.runIfActive(
      xdr, true, []() { return true; },
      [&](uint32_t) -> bool {
        retuneCount++;
        return true;
      },
      [&](uint8_t *, size_t maxSamples) -> size_t {
        readCount++;
        // The first two tuned centers produce no data (forcing their channels
        // into the fallback path); later centers and the fallback batch read
        // normally. Gating on the retune index keeps this robust to the number
        // of post-retune settle reads the engine performs.
        if (retuneCount <= 2) {
          return 0;
        }
        return std::min(maxSamples, kSamples);
      },
      [](const uint8_t *, size_t) {}, std::chrono::milliseconds(0),
      iqBuffer.data(), kSamples, kSampleRateHz, 0, 0.0, sdrConfig,
      [](uint32_t, int) {});

  REQUIRE(ran);
  // The sweep retunes once per capture, and the first two captures produce
  // no data, so the channels they covered have to be picked up by the
  // fallback path.
  //
  // The fallback batches the contiguous uncovered points into the largest
  // span one retune can cover instead of retuning once per point. The two
  // failed captures leave roughly 18 fine points uncovered; batching them
  // needs 2 retunes, so the sweep retunes about 6 times in total. A
  // per-point fallback would need more than 20.
  REQUIRE(retuneCount > 2);
  REQUIRE(retuneCount <= 8);

  std::lock_guard<std::mutex> lock(xdr.m_scanMutex);
  REQUIRE(!xdr.m_scanQueue.empty());
  // Every channel was measured and none of them holds a station, so every
  // one carries the "no signal" value.
  REQUIRE(xdr.m_scanQueue.back().second ==
          "U87500=0.0,87600=0.0,87700=0.0,87800=0.0,87900=0.0,88000=0.0,");
}

TEST_CASE("ScanEngine flushes the source buffer after every retune",
          "[scan_engine][xdr]") {
  XDRServer xdr;
  xdr.setVerboseLogging(false);

  xdr.m_scanStartKHz = 87500;
  xdr.m_scanStopKHz = 88000;
  xdr.m_scanStepKHz = 100;
  xdr.m_scanBandwidthHz = 56000;
  xdr.m_scanAntenna = 0;
  xdr.m_scanContinuous = false;
  xdr.m_scanStartPending = true;

  ScanEngine scan;
  std::atomic<int> requestedBandwidthHz{0};
  std::atomic<bool> pendingBandwidth{false};
  scan.handleControl(xdr, 90000000U, 56000, true, false, requestedBandwidthHz,
                     pendingBandwidth, [](uint32_t, int) {});

  constexpr uint32_t kSampleRateHz = 256000;
  constexpr size_t kSamples = 16384;
  std::vector<uint8_t> iqBuffer(kSamples * 2, 127);

  int retuneCount = 0;
  int flushCount = 0;
  Config::SDRSection sdrConfig = Config::SDRSection{};
  const bool ran = scan.runIfActive(
      xdr, true, []() { return true; },
      [&](uint32_t) -> bool {
        retuneCount++;
        return true;
      },
      [&](uint8_t *, size_t maxSamples) -> size_t {
        return std::min(maxSamples, kSamples);
      },
      [](const uint8_t *, size_t) {}, std::chrono::milliseconds(0),
      iqBuffer.data(), kSamples, kSampleRateHz, 0, 0.0, sdrConfig,
      [](uint32_t, int) {}, [&]() { flushCount++; });

  REQUIRE(ran);
  // Every retune must be followed by a flush so the FFT never reads the stale
  // pre-retune backlog (which otherwise mis-bins a station by one sweep step).
  REQUIRE(retuneCount > 0);
  REQUIRE(flushCount == retuneCount);
}

TEST_CASE("ScanEngine publishes the spectrum incrementally during the sweep",
          "[scan_engine][xdr]") {
  XDRServer xdr;
  xdr.setVerboseLogging(false);

  xdr.m_scanStartKHz = 87500;
  xdr.m_scanStopKHz = 88500;
  xdr.m_scanStepKHz = 100;
  xdr.m_scanBandwidthHz = 56000;
  xdr.m_scanAntenna = 0;
  xdr.m_scanContinuous = false;
  xdr.m_scanStartPending = true;

  ScanEngine scan;
  std::atomic<int> requestedBandwidthHz{0};
  std::atomic<bool> pendingBandwidth{false};
  scan.handleControl(xdr, 90000000U, 56000, true, false, requestedBandwidthHz,
                     pendingBandwidth, [](uint32_t, int) {});

  constexpr uint32_t kSampleRateHz = 256000;
  constexpr size_t kSamples = 16384;
  std::vector<uint8_t> iqBuffer(kSamples * 2, 127);

  int retuneCount = 0;
  Config::SDRSection sdrConfig = Config::SDRSection{};
  const bool ran = scan.runIfActive(
      xdr, true, []() { return true; },
      [&](uint32_t) -> bool {
        retuneCount++;
        return true;
      },
      [&](uint8_t *, size_t maxSamples) -> size_t {
        return std::min(maxSamples, kSamples);
      },
      [](const uint8_t *, size_t) {}, std::chrono::milliseconds(0),
      iqBuffer.data(), kSamples, kSampleRateHz, 0, 0.0, sdrConfig,
      [](uint32_t, int) {});

  REQUIRE(ran);
  REQUIRE(retuneCount > 1);

  std::lock_guard<std::mutex> lock(xdr.m_scanMutex);
  // One line per retune plus a final one, so the client sees the plot fill
  // in instead of an empty graph for the whole sweep. Before this the scan
  // emitted exactly one line, at the very end.
  REQUIRE(xdr.m_scanQueue.size() > 1);

  // Every line describes the SAME grid: the xdr-gtk client replaces its
  // dataset on each line and never merges, so a line that carried only the
  // measured prefix would make the client's x-axis shrink as the sweep grew.
  const size_t expectedChannels = 11; // 87500..88500 step 100
  for (const auto &entry : xdr.m_scanQueue) {
    const std::string &line = entry.second;
    REQUIRE(line.front() == 'U');
    const std::map<int, float> values = parseScanLine(line);
    REQUIRE(values.size() == expectedChannels);
    // The trailing comma is mandatory: the client's parser sizes its array
    // from the comma count and drops the last entry without it.
    REQUIRE(line.back() == ',');
    // The grid is fixed: it starts at the requested frequency and steps by
    // the requested amount.
    REQUIRE(values.begin()->first == 87500);
    auto it = values.begin();
    ++it;
    REQUIRE(it->first == 87600);
  }
}

TEST_CASE("ScanEngine widens the step when the grid would overflow one line",
          "[scan_engine][xdr]") {
  XDRServer xdr;
  xdr.setVerboseLogging(false);

  // A 5 kHz step over the whole band is 4100 channels. The reference client
  // reads into a 10000-byte buffer and drops the overflow, which would cut
  // the line mid-entry and lose the trailing comma -- the client then
  // discards the last entry and the plot ends short.
  xdr.m_scanStartKHz = 87500;
  xdr.m_scanStopKHz = 108000;
  xdr.m_scanStepKHz = 5;
  xdr.m_scanBandwidthHz = 56000;
  xdr.m_scanAntenna = 0;
  xdr.m_scanContinuous = false;
  xdr.m_scanStartPending = true;

  ScanEngine scan;
  std::atomic<int> requestedBandwidthHz{0};
  std::atomic<bool> pendingBandwidth{false};
  scan.handleControl(xdr, 90000000U, 56000, true, false, requestedBandwidthHz,
                     pendingBandwidth, [](uint32_t, int) {});

  constexpr uint32_t kSampleRateHz = 256000;
  constexpr size_t kSamples = 16384;
  std::vector<uint8_t> iqBuffer(kSamples * 2, 127);

  Config::SDRSection sdrConfig = Config::SDRSection{};
  const bool ran = scan.runIfActive(
      xdr, true, []() { return true; },
      [](uint32_t) -> bool { return true; },
      [&](uint8_t *, size_t maxSamples) -> size_t {
        return std::min(maxSamples, kSamples);
      },
      [](const uint8_t *, size_t) {}, std::chrono::milliseconds(0),
      iqBuffer.data(), kSamples, kSampleRateHz, 0, 0.0, sdrConfig,
      [](uint32_t, int) {});

  REQUIRE(ran);

  std::lock_guard<std::mutex> lock(xdr.m_scanMutex);
  REQUIRE(!xdr.m_scanQueue.empty());
  const std::map<int, float> values = parseScanLine(xdr.m_scanQueue.back().second);
  REQUIRE(values.size() <= 700);
  // The line must fit the client's receive buffer.
  REQUIRE(xdr.m_scanQueue.back().second.size() < 10000);
}
