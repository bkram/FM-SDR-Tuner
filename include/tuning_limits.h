#ifndef TUNING_LIMITS_H
#define TUNING_LIMITS_H

#include <cstdint>

namespace fm_tuner {

constexpr uint32_t kFmBroadcastMinFreqKHz = 64000U;
constexpr uint32_t kFmBroadcastMaxFreqKHz = 108000U;
constexpr uint32_t kFmBroadcastMinFreqHz = kFmBroadcastMinFreqKHz * 1000U;
constexpr uint32_t kFmBroadcastMaxFreqHz = kFmBroadcastMaxFreqKHz * 1000U;

constexpr bool isValidFmBroadcastFreqKHz(uint32_t freqKHz) {
  return freqKHz >= kFmBroadcastMinFreqKHz &&
         freqKHz <= kFmBroadcastMaxFreqKHz;
}

constexpr bool isValidFmBroadcastFreqHz(uint32_t freqHz) {
  return freqHz >= kFmBroadcastMinFreqHz &&
         freqHz <= kFmBroadcastMaxFreqHz;
}

} // namespace fm_tuner

#endif
