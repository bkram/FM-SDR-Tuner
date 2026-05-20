#include "catch_compat.h"

#include <chrono>

#include "adaptive_bandwidth.h"

using fm_tuner::AdaptiveBandwidthMode;
using fm_tuner::AdaptiveBandwidthState;
using fm_tuner::applyAdaptiveBandwidthHysteresis;
using fm_tuner::pickAdaptiveBandwidthHz;

TEST_CASE("Adaptive bandwidth Off returns zero", "[adaptive_bandwidth]") {
  REQUIRE(pickAdaptiveBandwidthHz(AdaptiveBandwidthMode::Off, 30.0) == 0);
  REQUIRE(pickAdaptiveBandwidthHz(AdaptiveBandwidthMode::Off, -5.0) == 0);
}

TEST_CASE("Adaptive bandwidth conservative monotonically widens with SNR",
          "[adaptive_bandwidth]") {
  const int weak = pickAdaptiveBandwidthHz(AdaptiveBandwidthMode::Conservative, 5.0);
  const int mid = pickAdaptiveBandwidthHz(AdaptiveBandwidthMode::Conservative, 15.0);
  const int strong = pickAdaptiveBandwidthHz(AdaptiveBandwidthMode::Conservative, 30.0);
  REQUIRE(weak > 0);
  REQUIRE(mid > weak);
  REQUIRE(strong > mid);
}

TEST_CASE("Adaptive bandwidth aggressive narrows further than conservative on weak signals",
          "[adaptive_bandwidth]") {
  const int weakAgg =
      pickAdaptiveBandwidthHz(AdaptiveBandwidthMode::Aggressive, 1.0);
  const int weakCons =
      pickAdaptiveBandwidthHz(AdaptiveBandwidthMode::Conservative, 1.0);
  REQUIRE(weakAgg > 0);
  REQUIRE(weakAgg < weakCons);
}

TEST_CASE("Adaptive bandwidth rejects non-finite SNR", "[adaptive_bandwidth]") {
  const double nanVal = std::nan("");
  REQUIRE(
      pickAdaptiveBandwidthHz(AdaptiveBandwidthMode::Conservative, nanVal) == 0);
  REQUIRE(
      pickAdaptiveBandwidthHz(AdaptiveBandwidthMode::Aggressive, nanVal) == 0);
}

TEST_CASE("Hysteresis blocks rapid bandwidth changes", "[adaptive_bandwidth]") {
  AdaptiveBandwidthState state{};
  const auto t0 = std::chrono::steady_clock::time_point{} +
                  std::chrono::seconds(10);
  // First change is allowed (state has never committed) and exceeds minStep.
  const int first = applyAdaptiveBandwidthHysteresis(state, 194000, 95000, t0);
  REQUIRE(first == 194000);
  REQUIRE(state.lastTargetHz == 194000);

  // Immediate second proposal must be suppressed by the min-interval gate.
  const auto t1 = t0 + std::chrono::milliseconds(500);
  const int second = applyAdaptiveBandwidthHysteresis(state, 95000, 194000, t1);
  REQUIRE(second == 0);

  // After the interval elapses, a meaningfully-different proposal commits.
  const auto t2 = t0 + std::chrono::milliseconds(2500);
  const int third = applyAdaptiveBandwidthHysteresis(state, 95000, 194000, t2);
  REQUIRE(third == 95000);
}

TEST_CASE("Hysteresis ignores tiny bandwidth nudges", "[adaptive_bandwidth]") {
  AdaptiveBandwidthState state{};
  state.lastTargetHz = 130000;
  state.lastChange = std::chrono::steady_clock::time_point{};
  const auto t = std::chrono::steady_clock::time_point{} +
                 std::chrono::seconds(60);
  // 10 kHz delta is below the default 20 kHz min-step threshold.
  const int proposed =
      applyAdaptiveBandwidthHysteresis(state, 140000, 130000, t);
  REQUIRE(proposed == 0);
}
