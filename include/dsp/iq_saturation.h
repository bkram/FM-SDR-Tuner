#ifndef FM_TUNER_DSP_IQ_SATURATION_H
#define FM_TUNER_DSP_IQ_SATURATION_H

#include <cstdint>

namespace fm_tuner::dsp {

// RTL-SDR delivers 8-bit unsigned I/Q. The literal min/max (0/255) are the
// only values that are *certainly* clipped, but the second-from-edge codes
// behave indistinguishably under any post-ADC rounding/dithering, so we treat
// them as saturated for both clip-ratio metering and demod-side overload
// detection. Keep this in one place so the meter and the demod can't drift
// to different thresholds.
inline constexpr std::uint8_t kRtlSdrIqLowSaturated = 1;
inline constexpr std::uint8_t kRtlSdrIqHighSaturated = 254;

inline constexpr bool isRtlSdrIqByteSaturated(std::uint8_t b) {
  return b <= kRtlSdrIqLowSaturated || b >= kRtlSdrIqHighSaturated;
}

} // namespace fm_tuner::dsp

#endif
