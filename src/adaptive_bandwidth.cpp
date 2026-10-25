#include "adaptive_bandwidth.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>

namespace fm_tuner {

namespace {

struct BandwidthStep {
  double minSnrDb; // the SNR must exceed this to select the step
  int hz;
};

// Widest first; the last step is the floor and has no threshold.
constexpr BandwidthStep kAggressiveSteps[] = {
    {25.0, 200000}, {15.0, 130000}, {8.0, 95000}, {3.0, 56000}, {0.0, 36000}};
// Conservative: stay wide for as long as the SNR allows.
constexpr BandwidthStep kConservativeSteps[] = {
    {20.0, 194000}, {10.0, 142000}, {0.0, 95000}};

template <std::size_t N>
int pickStep(const BandwidthStep (&steps)[N], double snrDb,
             int currentAppliedHz, double halfBandDb) {
  for (std::size_t i = 0; i + 1 < N; i++) {
    // Already at this step or wider: narrow only once the SNR falls below
    // the band. Narrower: widen only once it rises above it.
    const double threshold = (currentAppliedHz >= steps[i].hz)
                                 ? steps[i].minSnrDb - halfBandDb
                                 : steps[i].minSnrDb + halfBandDb;
    if (snrDb > threshold) {
      return steps[i].hz;
    }
  }
  return steps[N - 1].hz;
}

} // namespace

int pickAdaptiveBandwidthHz(AdaptiveBandwidthMode mode, double snrDb) {
  return pickAdaptiveBandwidthHz(mode, snrDb, 0, 0.0);
}

int pickAdaptiveBandwidthHz(AdaptiveBandwidthMode mode, double snrDb,
                            int currentAppliedHz, double hysteresisDb) {
  if (mode == AdaptiveBandwidthMode::Off) {
    return 0;
  }
  if (!std::isfinite(snrDb)) {
    // No usable channel-power estimate — leave bandwidth alone.
    return 0;
  }
  const double halfBandDb = std::max(0.0, hysteresisDb) / 2.0;
  if (mode == AdaptiveBandwidthMode::Aggressive) {
    return pickStep(kAggressiveSteps, snrDb, currentAppliedHz, halfBandDb);
  }
  return pickStep(kConservativeSteps, snrDb, currentAppliedHz, halfBandDb);
}

int applyAdaptiveBandwidthHysteresis(
    AdaptiveBandwidthState &state, int proposedHz, int currentAppliedHz,
    std::chrono::steady_clock::time_point now, int minIntervalMs,
    int minStepHz) {
  if (proposedHz <= 0) {
    return 0;
  }
  if (proposedHz == state.lastTargetHz) {
    return 0;
  }
  const auto elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(now - state.lastChange);
  if (elapsed.count() < minIntervalMs && state.lastChange.time_since_epoch().count() != 0) {
    return 0;
  }
  if (std::abs(proposedHz - currentAppliedHz) < minStepHz) {
    return 0;
  }
  state.lastChange = now;
  state.lastTargetHz = proposedHz;
  return proposedHz;
}

} // namespace fm_tuner
