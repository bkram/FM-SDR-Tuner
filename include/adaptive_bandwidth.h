#ifndef FM_TUNER_ADAPTIVE_BANDWIDTH_H
#define FM_TUNER_ADAPTIVE_BANDWIDTH_H

#include <chrono>

namespace fm_tuner {

enum class AdaptiveBandwidthMode { Off = 0, Conservative = 1, Aggressive = 2 };

struct AdaptiveBandwidthState {
  std::chrono::steady_clock::time_point lastChange{};
  int lastTargetHz = 0;
};

// Pure policy. Returns the channel bandwidth (Hz) the controller would like
// given the current SNR and the chosen mode. Returns 0 if the mode is Off
// (caller should leave bandwidth alone).
//
// Conservative: 3-band, 95-200 kHz. Errs on the side of preserving stereo
//   quality; only narrows on genuinely weak SNR.
// Aggressive:   5-band, 36-200 kHz. Will narrow aggressively to suppress
//   adjacent-channel splatter in crowded RF environments at the cost of
//   stereo image and HF response.
int pickAdaptiveBandwidthHz(AdaptiveBandwidthMode mode, double snrDb);

// Applies hysteresis: only allows changes every minIntervalMs ms, and only
// when the target differs from the previous target by at least minStepHz.
// Returns the bandwidth to apply (or 0 if no change should be made).
// Updates `state` if a change is committed.
int applyAdaptiveBandwidthHysteresis(
    AdaptiveBandwidthState &state, int proposedHz, int currentAppliedHz,
    std::chrono::steady_clock::time_point now, int minIntervalMs = 2000,
    int minStepHz = 20000);

} // namespace fm_tuner

#endif
