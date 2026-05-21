#ifndef FM_TUNER_DSP_SQUELCH_H
#define FM_TUNER_DSP_SQUELCH_H

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace fm_tuner::dsp {

// Channel-power squelch with hysteresis + soft ramp. When the post-DSP
// channel power dBFS estimate falls below `openDbfs`, the audio output is
// faded to silence over a few tens of ms; when it rises above
// `openDbfs + hysteresisDb`, the audio fades back in. Hysteresis avoids
// flutter at the threshold; the ramp avoids clicks.
//
// Reference: sdr-j-fm's squelchClass.h — the design intent is the same
// (mute empty / noise-floor channels during scan, DX, or unattended
// operation). Default openDbfs is effectively "always open" so the
// squelch is a no-op until a user lowers the threshold in the INI.
class Squelch {
public:
  Squelch() = default;

  // openDbfs: threshold below which audio is faded out. Use a very low
  //   value (e.g. -120) to disable; broadcast-relevant range is -90..-30.
  // hysteresisDb: how much above `openDbfs` the signal must be to re-open
  //   after a close. 3 dB is typical.
  // attackReleaseSec: ramp time for the audio fade in and out.
  void configure(float openDbfs, float hysteresisDb, float attackReleaseSec,
                 int sampleRate) {
    m_openDbfs = openDbfs;
    m_closeDbfs = openDbfs;
    m_hysteresisDb = std::max(0.0f, hysteresisDb);
    const float seconds = std::max(0.001f, attackReleaseSec);
    const float fs = static_cast<float>(std::max(1, sampleRate));
    // First-order time constant: alpha = 1 - exp(-1 / (tau · Fs))
    m_rampAlpha = 1.0f - std::exp(-1.0f / (seconds * fs));
    m_gain = 1.0f;
    m_isOpen = true;
  }

  // Update the gate decision from a per-block channel power estimate.
  // Stateless w.r.t. samples — call once per processing block (we already
  // compute channelPowerDbfs in DspPipeline::Result).
  void updateGate(double channelPowerDbfs) {
    if (m_openDbfs <= -119.0f) {
      // Effectively disabled — always open.
      m_isOpen = true;
      return;
    }
    if (!std::isfinite(channelPowerDbfs)) {
      // No estimate yet — stay in current state to avoid spurious mutes.
      return;
    }
    const float dbfs = static_cast<float>(channelPowerDbfs);
    if (m_isOpen) {
      // Closing threshold is the configured open level.
      if (dbfs < m_openDbfs) {
        m_isOpen = false;
      }
    } else {
      // Reopen requires `hysteresisDb` above the open threshold.
      if (dbfs > (m_openDbfs + m_hysteresisDb)) {
        m_isOpen = true;
      }
    }
  }

  // Apply the gate to a stereo audio buffer in-place. Gain ramps toward
  // the target (1.0 when open, 0.0 when closed) per sample using the
  // alpha computed in `configure`.
  void process(float *left, float *right, std::size_t numSamples) {
    if (!left || !right || numSamples == 0) {
      return;
    }
    if (m_openDbfs <= -119.0f) {
      // Disabled — no work, no state change.
      return;
    }
    const float target = m_isOpen ? 1.0f : 0.0f;
    for (std::size_t i = 0; i < numSamples; ++i) {
      m_gain += m_rampAlpha * (target - m_gain);
      left[i] *= m_gain;
      right[i] *= m_gain;
    }
  }

  bool isOpen() const { return m_isOpen; }
  float currentGain() const { return m_gain; }

private:
  float m_openDbfs = -120.0f; // disabled sentinel
  float m_closeDbfs = -120.0f;
  float m_hysteresisDb = 3.0f;
  float m_rampAlpha = 0.001f;
  float m_gain = 1.0f;
  bool m_isOpen = true;
};

} // namespace fm_tuner::dsp

#endif
