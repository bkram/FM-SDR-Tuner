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

namespace {

std::vector<uint8_t> makeIqTone(size_t samples, uint32_t sampleRateHz,
                                float toneHz, float amplitude) {
  std::vector<uint8_t> iq(samples * 2, 127);
  constexpr float kPi = 3.14159265358979323846f;
  for (size_t i = 0; i < samples; i++) {
    const float phase =
        2.0f * kPi * toneHz * (static_cast<float>(i) / sampleRateHz);
    const float iSample = std::cos(phase) * amplitude;
    const float qSample = std::sin(phase) * amplitude;
    iq[i * 2] = static_cast<uint8_t>(
        std::clamp(std::lround((iSample * 127.5f) + 127.5f), 0L, 255L));
    iq[i * 2 + 1] = static_cast<uint8_t>(
        std::clamp(std::lround((qSample * 127.5f) + 127.5f), 0L, 255L));
  }
  return iq;
}

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

  Config::SDRSection sdrConfig{};
  const bool ran = scan.runIfActive(
      xdr, true, []() { return true; },
      [](uint32_t) -> bool { return true; },
      [](uint8_t *, size_t) -> size_t { return 0; },
      [](const uint8_t *, size_t) {}, std::chrono::milliseconds(0),
      iqBuffer.data(), 4096, 256000, 0, 0.0, sdrConfig,
      [](uint32_t, int) {});

  REQUIRE(ran);

  std::lock_guard<std::mutex> lock(xdr.m_scanMutex);
  REQUIRE(xdr.m_scanQueue.size() == 1);
  const std::string &line = xdr.m_scanQueue.back().second;
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
  // by the Nyquist constraint, so first center = startKHz*1000 + 108800.
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
  // First center = startKHz*1000 + min(Fs*0.85/2, Fs/2 - channelBW/2)
  //              = 87500000 + min(108800, 128000-20000)
  //              = 87500000 + 108000 = 87608000
  constexpr int64_t kCenterHz = 87608000;
  constexpr float kToneHz = static_cast<float>(87600000 - kCenterHz);
  std::vector<uint8_t> iqBuffer = makeIqTone(kSamples, kSampleRateHz, kToneHz, 0.70f);

  int readsRemaining = 3; // one settle discard + two FFT averages
  Config::SDRSection sdrConfig{};
  const bool ran = scan.runIfActive(
      xdr, true, []() { return true; }, [](uint32_t) -> bool { return true; },
      [&](uint8_t *dest, size_t maxSamples) -> size_t {
        if (readsRemaining <= 0) {
          return 0;
        }
        const size_t copySamples = std::min(maxSamples, kSamples);
        std::memcpy(dest, iqBuffer.data(), copySamples * 2);
        readsRemaining--;
        return copySamples;
      },
      [](const uint8_t *, size_t) {}, std::chrono::milliseconds(0),
      iqBuffer.data(), kSamples, kSampleRateHz, 0, 0.0, sdrConfig,
      [](uint32_t, int) {});

  REQUIRE(ran);

  std::lock_guard<std::mutex> lock(xdr.m_scanMutex);
  REQUIRE(xdr.m_scanQueue.size() == 1);
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
  Config::SDRSection sdrConfig{};
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
  // 4 main-loop retunes + 1 fallback batch covering channels 87500-87700.
  // Channel 87800 is picked up by the main loop because the coverage-capped
  // centerStepHz keeps consecutive captures overlapping.
  REQUIRE(retuneCount == 5);

  std::lock_guard<std::mutex> lock(xdr.m_scanMutex);
  REQUIRE(xdr.m_scanQueue.size() == 1);
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
  Config::SDRSection sdrConfig{};
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
