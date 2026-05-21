#include "dsp/multipath_eq.h"

#include <algorithm>
#include <cmath>

namespace fm_tuner::dsp {

namespace {
// Step sizes for the dispersion-CMA form. Calibrated so a strong multipath
// signal converges in well under a second; on a clean signal the dispersion
// target makes the error near-zero and these values stay safe.
constexpr float kMuLight = 5.0e-5f;
constexpr float kMuAggressive = 2.0e-4f;

// Leak strength toward the centered-delta initialization. After each LMS
// update we apply `taps *= (1 - leak)` and add `leak` back to the centre tap,
// which biases CMA toward delta-like (linear-phase, near-transparent)
// solutions. Without this, CMA on FM has a documented phase-ambiguity issue
// that lets the filter converge to a constant-envelope but phase-distorting
// solution. Aggressive mode allows the leak to be weaker so it can chase
// stronger multipath.
constexpr float kLeakLight = 5.0e-5f;
constexpr float kLeakAggressive = 1.0e-5f;

// EMA constant for the running input |x|² mean and for the diagnostic
// envelope-error trace. 1/65536 ≈ 256 ms time constant at 256 kHz — slow
// enough that the target tracks long-term envelope, not per-sample wiggle.
constexpr float kInputMeanAlpha = 1.0f / 65536.0f;
constexpr float kEnvErrorAlpha = 1.0f / 256.0f;

float muForMode(MultipathEqMode mode) {
  switch (mode) {
  case MultipathEqMode::Off:
    return 0.0f;
  case MultipathEqMode::Light:
    return kMuLight;
  case MultipathEqMode::Aggressive:
    return kMuAggressive;
  }
  return 0.0f;
}

float leakForMode(MultipathEqMode mode) {
  switch (mode) {
  case MultipathEqMode::Off:
    return 0.0f;
  case MultipathEqMode::Light:
    return kLeakLight;
  case MultipathEqMode::Aggressive:
    return kLeakAggressive;
  }
  return 0.0f;
}
} // namespace

MultipathEqualizer::MultipathEqualizer() = default;

void MultipathEqualizer::init(MultipathEqMode mode, std::uint32_t taps,
                              float sampleRate) {
  m_mode = mode;
  m_tapCount = std::max<std::uint32_t>(3U, taps);
  if ((m_tapCount % 2U) == 0U) {
    m_tapCount += 1U; // keep symmetric for a centered delta initial response
  }
  m_sampleRate = (sampleRate > 0.0f) ? sampleRate : 1.0f;
  m_mu = muForMode(mode);
  m_leak = leakForMode(mode);
  m_targetPrimed = false;
  m_inputMeanSqMag = 0.0f;
  rebuildTaps();
}

void MultipathEqualizer::setMode(MultipathEqMode mode) {
  if (mode == m_mode) {
    return;
  }
  m_mode = mode;
  m_mu = muForMode(mode);
  m_leak = leakForMode(mode);
  m_targetPrimed = false;
  m_inputMeanSqMag = 0.0f;
  // Mode change implies "the user wants a different policy" — best to reset
  // the taps so a stale adaptation under a different μ doesn't bleed through.
  rebuildTaps();
}

void MultipathEqualizer::reset() {
  rebuildTaps();
  m_envelopeError = 0.0f;
  m_targetPrimed = false;
  m_inputMeanSqMag = 0.0f;
}

void MultipathEqualizer::rebuildTaps() {
  if (m_tapCount == 0U) {
    m_tapCount = 17U;
  }
  m_taps.assign(m_tapCount, std::complex<float>(0.0f, 0.0f));
  m_history.assign(m_tapCount, std::complex<float>(0.0f, 0.0f));
  // Initialize to a delta at the centre tap so the equalizer starts as a
  // pure unit delay (transparent). CMA adapts away from this only when a
  // strong constant-modulus reference is available.
  m_taps[m_tapCount / 2U] = std::complex<float>(1.0f, 0.0f);
  m_writePos = 0;
}

std::complex<float> MultipathEqualizer::execute(std::complex<float> input) {
  if (m_mode == MultipathEqMode::Off || m_taps.empty()) {
    return input;
  }

  // Track input envelope power (|x|²) with a slow EMA. The first sample
  // primes the average to avoid a long pull-up transient from zero.
  const float xMagSq =
      input.real() * input.real() + input.imag() * input.imag();
  if (!m_targetPrimed) {
    m_inputMeanSqMag = xMagSq;
    m_targetPrimed = true;
  } else {
    m_inputMeanSqMag += kInputMeanAlpha * (xMagSq - m_inputMeanSqMag);
  }

  // Newest sample at m_writePos.
  m_history[m_writePos] = input;

  // Convolve. We index history backwards from writePos so tap[0] is the
  // newest sample, tap[N-1] is the oldest — matches a standard transversal
  // FIR (taps[k] multiplies x[n-k]).
  const std::uint32_t N = m_tapCount;
  std::complex<float> y(0.0f, 0.0f);
  for (std::uint32_t k = 0; k < N; ++k) {
    const std::uint32_t idx =
        (m_writePos + N - k) % N;
    y += m_taps[k] * m_history[idx];
  }

  // Dispersion-CMA update: minimize (|y|² - E[|x|²])². The target tracks the
  // input envelope so on a constant-envelope signal the error is near zero
  // and the filter stays put. Multipath shows up as envelope variation
  // around the running mean — that's what gets driven down.
  if (m_adaptEnabled && m_mu > 0.0f) {
    const float yMagSq = y.real() * y.real() + y.imag() * y.imag();
    const float envErr = yMagSq - m_inputMeanSqMag;
    m_envelopeError +=
        kEnvErrorAlpha * (std::abs(envErr) - m_envelopeError);

    const std::complex<float> errTimesY = y * envErr;
    const std::uint32_t centreIdx = N / 2U;
    for (std::uint32_t k = 0; k < N; ++k) {
      const std::uint32_t idx =
          (m_writePos + N - k) % N;
      const std::complex<float> grad = errTimesY * std::conj(m_history[idx]);
      // CMA step + leak toward delta: every tap shrinks by (1 - leak),
      // and the centre tap is then nudged back toward 1. Pulls the filter
      // back to a pure pass-through when the LMS gradient is small —
      // preventing CMA's known phase-ambiguity wandering on clean FM.
      m_taps[k] = m_taps[k] * (1.0f - m_leak) - m_mu * grad;
      if (k == centreIdx) {
        m_taps[k] += m_leak * std::complex<float>(1.0f, 0.0f);
      }
    }
  }

  // Advance ring write pointer for next sample.
  m_writePos = (m_writePos + 1U) % m_tapCount;
  return y;
}

} // namespace fm_tuner::dsp
