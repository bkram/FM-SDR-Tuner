#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <vector>

#define private public
#include "xdr_server.h"
#undef private

#include "scan_engine.h"

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
      [](uint32_t) {},
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
