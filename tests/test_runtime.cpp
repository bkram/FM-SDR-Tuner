#include "catch_compat.h"

#include <atomic>

#include "dsp/runtime.h"

TEST_CASE("resetReasonName maps all enum values", "[runtime]") {
  using fm_tuner::dsp::ResetReason;
  using fm_tuner::dsp::resetReasonName;

  REQUIRE(std::string(resetReasonName(ResetReason::Start)) == "start");
  REQUIRE(std::string(resetReasonName(ResetReason::Stop)) == "stop");
  REQUIRE(std::string(resetReasonName(ResetReason::Retune)) == "retune");
  REQUIRE(std::string(resetReasonName(ResetReason::ScanRestore)) ==
          "scan_restore");
}

TEST_CASE("Runtime enforces minimum block size and invokes reset handlers",
          "[runtime]") {
  fm_tuner::dsp::Runtime runtime(0, false);
  REQUIRE(runtime.blockSize() == 1);

  std::atomic<int> calls{0};
  runtime.addResetHandler([&]() { calls.fetch_add(1, std::memory_order_relaxed); });
  runtime.reset(fm_tuner::dsp::ResetReason::Start);
  runtime.reset(fm_tuner::dsp::ResetReason::Retune);

  REQUIRE(calls.load(std::memory_order_relaxed) == 2);
}
