#include "adaptive_bandwidth.h"

#include <chrono>
#include <cmath>
#include <cstdlib>

namespace fm_tuner {

int pickAdaptiveBandwidthHz(AdaptiveBandwidthMode mode, double snrDb) {
  if (mode == AdaptiveBandwidthMode::Off) {
    return 0;
  }
  if (!std::isfinite(snrDb)) {
    // No usable channel-power estimate — leave bandwidth alone.
    return 0;
  }
  if (mode == AdaptiveBandwidthMode::Aggressive) {
    if (snrDb > 25.0) return 200000;
    if (snrDb > 15.0) return 130000;
    if (snrDb > 8.0)  return 95000;
    if (snrDb > 3.0)  return 56000;
    return 36000;
  }
  // Conservative: stay wide for as long as the SNR allows.
  if (snrDb > 20.0) return 194000;
  if (snrDb > 10.0) return 142000;
  return 95000;
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
