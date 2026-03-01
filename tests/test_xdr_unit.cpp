#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <string>

#define private public
#include "xdr_server.h"
#undef private

TEST_CASE("XDR processCommand updates state and invokes callbacks", "[xdr_unit]") {
  XDRServer xdr(7374);
  xdr.setVerboseLogging(false);

  std::atomic<uint32_t> tuned{0};
  std::atomic<int> volume{0};
  std::atomic<int> agc{0};
  std::atomic<int> sampleInterval{0};
  std::atomic<int> detector{0};
  std::atomic<bool> mono{false};
  std::atomic<int> startCalls{0};
  std::atomic<int> stopCalls{0};

  xdr.setFrequencyCallback([&](uint32_t f) { tuned = f; });
  xdr.setVolumeCallback([&](int v) { volume = v; });
  xdr.setAGCCallback([&](int a) {
    agc = a;
    return true;
  });
  xdr.setSamplingCallback([&](int i, int d) {
    sampleInterval = i;
    detector = d;
  });
  xdr.setForceMonoCallback([&](bool v) { mono = v; });
  xdr.setStartCallback([&]() { startCalls++; });
  xdr.setStopCallback([&]() { stopCalls++; });

  REQUIRE(xdr.processCommand("T101700", true, false) == "T101700");
  REQUIRE(tuned.load() == 101700000u);
  REQUIRE(xdr.getFrequency() == 101700000u);

  REQUIRE(xdr.processCommand("T101700000", true, false) == "T101700");
  REQUIRE(tuned.load() == 101700000u);

  REQUIRE(xdr.processCommand("Y77", true, false) == "Y77");
  REQUIRE(volume.load() == 77);
  REQUIRE(xdr.getVolume() == 77);

  REQUIRE(xdr.processCommand("A3", true, false) == "A3");
  REQUIRE(agc.load() == 3);
  REQUIRE(xdr.getAGCMode() == 3);

  REQUIRE(xdr.processCommand("I250,1", true, false) == "I250,1");
  REQUIRE(sampleInterval.load() == 250);
  REQUIRE(detector.load() == 1);

  REQUIRE(xdr.processCommand("B1", true, false) == "B1");
  REQUIRE(mono.load());

  REQUIRE(xdr.processCommand("x", true, false) == "OK");
  REQUIRE(xdr.processCommand("X", true, false) == "X");
  REQUIRE(startCalls.load() == 1);
  REQUIRE(stopCalls.load() == 1);
}

TEST_CASE("XDR scan commands are clamped and exposed via consumeScanStart",
          "[xdr_unit]") {
  XDRServer xdr;
  xdr.setVerboseLogging(false);

  REQUIRE(xdr.processCommand("Sa63000", true, false).empty());
  REQUIRE(xdr.processCommand("Sb200000", true, false).empty());
  REQUIRE(xdr.processCommand("Sc2", true, false).empty());
  REQUIRE(xdr.processCommand("Sw999999", true, false).empty());
  REQUIRE(xdr.processCommand("Sz99", true, false).empty());
  REQUIRE(xdr.processCommand("Sm", true, false).empty());

  XDRServer::ScanConfig cfg{};
  REQUIRE(xdr.consumeScanStart(cfg));
  REQUIRE(cfg.startKHz == 64000);
  REQUIRE(cfg.stopKHz == 120000);
  REQUIRE(cfg.stepKHz == 5);
  REQUIRE(cfg.bandwidthHz == 400000);
  REQUIRE(cfg.antenna == 9);
  REQUIRE(cfg.continuous);
}

TEST_CASE("XDR updateRDS suppresses groups with block B errors", "[xdr_unit]") {
  XDRServer xdr;
  xdr.setVerboseLogging(false);

  xdr.updateRDS(0x1111, 0xABCD, 0x2222, 0x3333, 0x00);
  xdr.updateRDS(0x1111, 0xBBBB, 0x4444, 0x5555, 0x10); // block B error

  std::lock_guard<std::mutex> lock(xdr.m_rdsMutex);
  bool sawClean = false;
  bool sawErrored = false;
  for (const auto &entry : xdr.m_rdsQueue) {
    if (entry.second == "RABCD2222333300") {
      sawClean = true;
    }
    if (entry.second == "RBBBB4444555510") {
      sawErrored = true;
    }
  }

  REQUIRE(sawClean);
  REQUIRE_FALSE(sawErrored);
}
