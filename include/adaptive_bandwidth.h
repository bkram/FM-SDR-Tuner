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

// The same policy with an SNR hysteresis band around each threshold, given
// the bandwidth currently applied. Staying at (or above) a band needs the SNR
// to fall hysteresisDb/2 below its threshold before narrowing; moving up to
// it needs the SNR hysteresisDb/2 above. An SNR hovering at a threshold then
// holds the current bandwidth instead of toggling every rate-limit interval.
int pickAdaptiveBandwidthHz(AdaptiveBandwidthMode mode, double snrDb,
                            int currentAppliedHz, double hysteresisDb = 3.0);

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
