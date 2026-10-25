#ifndef FM_TUNER_DSP_MPX_RESAMPLER_H
#define FM_TUNER_DSP_MPX_RESAMPLER_H

#include <algorithm>
#include <cstdint>

#include "dsp/liquid_primitives.h"

namespace fm_tuner::dsp {

// Sets up the resampler that converts the decoded MPX (at the IQ rate) to
// the rate of an MPX output (WAV file or audio device).
//
// liquid's defaults (half-length 12, cutoff 0.47 of the input rate) do not
// band-limit a downsample: converting 256 kHz to 192 kHz, a 100 kHz component
// came out at 92 kHz at full level, and 110 / 120 kHz folded to 82 / 72 kHz at
// -0.9 / -5.8 dB. When the output rate is lower than the input, the cutoff
// sits at 0.9 of the output Nyquist rate (86.4 kHz at 192 kHz out, well above
// the 60 kHz MPX baseband), with a long enough filter for a sharp transition.
inline void initMpxResampler(liquid::Resampler &resampler, uint32_t inputRate,
                             uint32_t outputRate) {
  constexpr std::uint32_t kHalfLength = 64;
  constexpr float kStopBandDb = 80.0f;
  constexpr std::uint32_t kPolyphaseFilters = 64;
  const float ratio =
      static_cast<float>(outputRate) / static_cast<float>(inputRate);
  const float cutoff = std::min(0.45f, 0.45f * ratio);
  resampler.init(ratio, kHalfLength, cutoff, kStopBandDb, kPolyphaseFilters);
}

} // namespace fm_tuner::dsp

#endif
