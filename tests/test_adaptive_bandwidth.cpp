#include "catch_compat.h"

#include <chrono>
#include <cmath>

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

TEST_CASE("Adaptive bandwidth holds still while the SNR hovers at a threshold",
          "[adaptive_bandwidth]") {
  // An SNR wobbling around the conservative 20 dB threshold used to switch
  // 194 <-> 142 kHz at every rate-limit interval (2 s). The hysteresis band
  // holds the bandwidth that is applied.
  for (int startHz : {194000, 142000}) {
    AdaptiveBandwidthState state{};
    int applied = startHz;
    int changes = 0;
    auto t = std::chrono::steady_clock::time_point{} + std::chrono::seconds(10);
    for (int step = 0; step < 20; step++) {
      const double snr = (step % 2 == 0) ? 19.2 : 20.8;
      const int proposed = pickAdaptiveBandwidthHz(
          AdaptiveBandwidthMode::Conservative, snr, applied);
      const int change =
          applyAdaptiveBandwidthHysteresis(state, proposed, applied, t);
      if (change > 0 && change != applied) {
        applied = change;
        changes++;
      }
      t += std::chrono::milliseconds(2500);
    }
    INFO("start " << startHz << " Hz: " << changes << " changes");
    CHECK(changes == 0);
    CHECK(applied == startHz);
  }
}

TEST_CASE("Adaptive bandwidth still moves once the SNR leaves the band",
          "[adaptive_bandwidth]") {
  using M = AdaptiveBandwidthMode;
  // Wide and staying wide down to threshold - 1.5 dB.
  REQUIRE(pickAdaptiveBandwidthHz(M::Conservative, 18.6, 194000) == 194000);
  REQUIRE(pickAdaptiveBandwidthHz(M::Conservative, 18.4, 194000) == 142000);
  // Narrow and widening only above threshold + 1.5 dB.
  REQUIRE(pickAdaptiveBandwidthHz(M::Conservative, 21.4, 142000) == 142000);
  REQUIRE(pickAdaptiveBandwidthHz(M::Conservative, 21.6, 142000) == 194000);
  // A deep fade still drops straight to the floor.
  REQUIRE(pickAdaptiveBandwidthHz(M::Conservative, 2.0, 194000) == 95000);
  REQUIRE(pickAdaptiveBandwidthHz(M::Aggressive, 30.0, 36000) == 200000);
  REQUIRE(pickAdaptiveBandwidthHz(M::Aggressive, 1.0, 200000) == 36000);
  // Without a current bandwidth the plain thresholds apply.
  REQUIRE(pickAdaptiveBandwidthHz(M::Conservative, 20.5) == 194000);
  REQUIRE(pickAdaptiveBandwidthHz(M::Conservative, 19.5) == 142000);
}
