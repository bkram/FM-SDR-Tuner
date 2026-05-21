#ifndef FM_TUNER_DSP_MULTIPATH_EQ_H
#define FM_TUNER_DSP_MULTIPATH_EQ_H

#include <complex>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace fm_tuner::dsp {

enum class MultipathEqMode : std::uint8_t { Off = 0, Light = 1, Aggressive = 2 };

// Complex-baseband constant-modulus equalizer (Godard 1980 / CMA, patent-free).
// FM has constant envelope, so multipath manifests as amplitude variation in
// the post-channel-FIR I/Q baseband. CMA minimizes (|y|² - R²)² to drive the
// envelope back to a constant, equivalently cancelling the additive ghost.
//
// Limitations:
//  - CMA can converge to local minima; gated by a stereo-lock external signal
//    so it only adapts when a strong, real signal is present.
//  - Step size is small to stay stable in the presence of FM signal dynamics.
//  - 16-tap default keeps latency low (~62 µs at 256 kHz) and preserves group
//    delay relative to the rest of the chain.
class MultipathEqualizer {
public:
  MultipathEqualizer();

  void init(MultipathEqMode mode, std::uint32_t taps, float sampleRate);
  void setMode(MultipathEqMode mode);
  void setAdaptEnabled(bool enabled) { m_adaptEnabled = enabled; }
  void reset();

  // Returns the equalized sample. If mode is Off, returns input unchanged.
  std::complex<float> execute(std::complex<float> input);

  // Diagnostics — exposed so tests and the signal-level logger can see how the
  // equalizer is behaving. Average over the most recent ~kEnvFilterTaps samples.
  float envelopeError() const { return m_envelopeError; }
  bool isActive() const { return m_mode != MultipathEqMode::Off; }

private:
  void rebuildTaps();

  MultipathEqMode m_mode = MultipathEqMode::Off;
  std::uint32_t m_tapCount = 0;
  float m_sampleRate = 1.0f;
  float m_mu = 0.0f;
  float m_leak = 0.0f; // bias taps back toward delta to fight CMA wandering
  bool m_adaptEnabled = false;
  bool m_targetPrimed = false;

  std::vector<std::complex<float>> m_taps;
  std::vector<std::complex<float>> m_history;
  std::uint32_t m_writePos = 0;
  // Running EMA of |x|². The CMA target tracks this so the equalizer is
  // dispersion-form (minimizes envelope variance) rather than absolute-level.
  // On a clean FM signal (already constant envelope) the error stays near
  // zero and the filter stays near the centered-delta initialization.
  float m_inputMeanSqMag = 0.0f;
  // EMA of |error| for diagnostics.
  float m_envelopeError = 0.0f;
};

} // namespace fm_tuner::dsp

#endif
