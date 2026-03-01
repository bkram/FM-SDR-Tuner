#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <string>

#define private public
#include "xdr_facade.h"
#undef private

TEST_CASE("XdrFacade wires callbacks into command state", "[xdr_facade]") {
  XDRServer server(7379);
  XdrCommandState state(88600000u, 11, 2, 0, 55, 1, false);
  XdrFacade facade(server, state,
                   {.verboseLogging = false,
                    .useSdrppGainStrategy = false,
                    .allowClientGainOverride = true});

  std::atomic<int> setVolumeCalls{0};
  std::atomic<int> startCalls{0};
  std::atomic<int> stopCalls{0};
  std::atomic<int> lastVolume{0};

  facade.configureServer("", false);
  facade.installCallbacks(
      [&](int volume) {
        setVolumeCalls.fetch_add(1, std::memory_order_relaxed);
        lastVolume.store(volume, std::memory_order_relaxed);
      },
      [&]() { startCalls.fetch_add(1, std::memory_order_relaxed); },
      [&]() { stopCalls.fetch_add(1, std::memory_order_relaxed); },
      [&](int custom) {
        return std::to_string((custom / 10) % 10) + std::to_string(custom % 10);
      });

  REQUIRE(server.processCommand("T101700", true, false) == "T101700");
  REQUIRE(state.requestedFrequencyHz.load(std::memory_order_relaxed) ==
          101700000u);
  REQUIRE(state.pendingFrequency.load(std::memory_order_acquire));

  REQUIRE(server.processCommand("Y80", true, false) == "Y80");
  REQUIRE(state.requestedVolume.load(std::memory_order_relaxed) == 80);
  REQUIRE(setVolumeCalls.load(std::memory_order_relaxed) == 1);
  REQUIRE(lastVolume.load(std::memory_order_relaxed) == 80);

  REQUIRE(server.processCommand("A3", true, false) == "A3");
  REQUIRE(state.requestedAGCMode.load(std::memory_order_relaxed) == 3);
  REQUIRE(state.pendingAGC.load(std::memory_order_relaxed));

  REQUIRE(server.processCommand("G10", true, false) == "G10");
  REQUIRE(state.requestedCustomGain.load(std::memory_order_relaxed) == 10);
  REQUIRE(state.pendingGain.load(std::memory_order_relaxed));

  REQUIRE(server.processCommand("W150000", true, false) == "W150000");
  REQUIRE(state.requestedBandwidthHz.load(std::memory_order_relaxed) == 150000);
  REQUIRE(state.pendingBandwidth.load(std::memory_order_relaxed));

  REQUIRE(server.processCommand("D2", true, false) == "D2");
  REQUIRE(state.requestedDeemphasis.load(std::memory_order_relaxed) == 2);

  REQUIRE(server.processCommand("B1", true, false) == "B1");
  REQUIRE(state.requestedForceMono.load(std::memory_order_relaxed));

  REQUIRE(server.processCommand("X", true, false) == "X");
  REQUIRE(server.processCommand("x", true, false) == "OK");
  REQUIRE(startCalls.load(std::memory_order_relaxed) == 1);
  REQUIRE(stopCalls.load(std::memory_order_relaxed) == 1);
}

TEST_CASE("XdrFacade respects gain override policy", "[xdr_facade]") {
  XDRServer server(7380);
  XdrCommandState state(88600000u, 11, 2, 0, 55, 1, false);
  XdrFacade facade(server, state,
                   {.verboseLogging = false,
                    .useSdrppGainStrategy = false,
                    .allowClientGainOverride = false});

  facade.configureServer("", false);
  facade.installCallbacks(
      [&](int) {}, [&]() {}, [&]() {},
      [&](int custom) {
        return std::to_string((custom / 10) % 10) + std::to_string(custom % 10);
      });

  REQUIRE(server.processCommand("G10", true, false) == "G00");
  REQUIRE(server.processCommand("A3", true, false) == "A2");
  REQUIRE_FALSE(state.pendingGain.load(std::memory_order_relaxed));
  REQUIRE_FALSE(state.pendingAGC.load(std::memory_order_relaxed));
  REQUIRE(state.requestedCustomGain.load(std::memory_order_relaxed) == 11);
  REQUIRE(state.requestedAGCMode.load(std::memory_order_relaxed) == 2);
}

TEST_CASE("XdrFacade callbacks remain valid after install scope ends",
          "[xdr_facade]") {
  XDRServer server(7381);
  XdrCommandState state(88600000u, 0, 0, 0, 50, 1, false);
  XdrFacade facade(server, state,
                   {.verboseLogging = false,
                    .useSdrppGainStrategy = false,
                    .allowClientGainOverride = true});

  std::atomic<int> startCalls{0};
  std::atomic<int> stopCalls{0};

  facade.configureServer("", false);
  {
    facade.installCallbacks(
        [&](int) {}, [&]() { startCalls.fetch_add(1, std::memory_order_relaxed); },
        [&]() { stopCalls.fetch_add(1, std::memory_order_relaxed); },
        [&](int custom) {
          return std::to_string((custom / 10) % 10) +
                 std::to_string(custom % 10);
        });
  }

  REQUIRE(server.processCommand("X", true, false) == "X");
  REQUIRE(server.processCommand("x", true, false) == "OK");
  REQUIRE(startCalls.load(std::memory_order_relaxed) == 1);
  REQUIRE(stopCalls.load(std::memory_order_relaxed) == 1);
}
