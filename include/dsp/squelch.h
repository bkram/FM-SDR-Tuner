#ifndef FM_TUNER_DSP_SQUELCH_H
#define FM_TUNER_DSP_SQUELCH_H

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

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

  // Adaptive "fade mute": instead of an absolute threshold, track a slowly
  // decaying reference of the channel power and mute when it drops `dropDb`
  // below that reference. FM is constant-envelope, so a channel-power drop is a
  // genuine RF fade (never quiet program audio) — this suppresses the demod
  // noise burst on a dropout and fades back in on recovery, without permanently
  // silencing weak stations (the reference decays so steady weak signals open).
  // updatesPerSec is the rate updateGate() is called (one per processing block).
  void configureFadeMute(float dropDb, float hysteresisDb,
                         float attackReleaseSec, int sampleRate,
                         float refDecayDbPerSec, float updatesPerSec) {
    m_adaptive = true;
    m_dropDb = std::max(3.0f, dropDb);
    m_hysteresisDb = std::max(0.0f, hysteresisDb);
    const float seconds = std::max(0.001f, attackReleaseSec);
    const float fs = static_cast<float>(std::max(1, sampleRate));
    m_rampAlpha = 1.0f - std::exp(-1.0f / (seconds * fs));
    m_refDecayPerUpdate =
        std::max(0.0f, refDecayDbPerSec) / std::max(1.0f, updatesPerSec);
    m_refInit = false;
    m_snrRefInit = false;
    m_gain = 1.0f;
    m_isOpen = true;
  }

  bool adaptiveEnabled() const { return m_adaptive; }

  // Update the gate decision from a per-block channel power estimate.
  // Stateless w.r.t. samples — call once per processing block (we already
  // compute channelPowerDbfs in DspPipeline::Result).
  //
  // demodSnrDb (optional) is the demod-domain hiss SNR. When finite and the
  // adaptive fade-mute is active, it acts as a second, symmetric fade trigger:
  // the gate closes when EITHER the channel power or the SNR drops `dropDb`
  // below its slowly-decaying reference, and reopens only when BOTH have
  // recovered. This catches quality collapses (interference / multipath bursts)
  // that leave channel power high. Pass NaN (the default) to disable it.
  void updateGate(double channelPowerDbfs,
                  float demodSnrDb = std::numeric_limits<float>::quiet_NaN()) {
    if (m_adaptive) {
      if (!std::isfinite(channelPowerDbfs)) {
        return;
      }
      const float dbfs = static_cast<float>(channelPowerDbfs);
      if (!m_refInit) {
        m_refDbfs = dbfs;
        m_refInit = true;
      }
      // Reference rises instantly to the current level and decays slowly, so a
      // sudden drop is detected as a fade while a steady (even weak) signal
      // keeps the gate open.
      m_refDbfs = std::max(dbfs, m_refDbfs - m_refDecayPerUpdate);
      const float openLevel = m_refDbfs - m_dropDb;
      bool powerFaded = dbfs < openLevel;
      bool powerRecovered = dbfs > openLevel + m_hysteresisDb;

      bool snrFaded = false;
      bool snrRecovered = true;
      if (std::isfinite(demodSnrDb)) {
        if (!m_snrRefInit) {
          m_snrRef = demodSnrDb;
          m_snrRefInit = true;
        }
        m_snrRef = std::max(demodSnrDb, m_snrRef - m_refDecayPerUpdate);
        const float snrOpenLevel = m_snrRef - m_dropDb;
        snrFaded = demodSnrDb < snrOpenLevel;
        snrRecovered = demodSnrDb > snrOpenLevel + m_hysteresisDb;
      }

      if (m_isOpen) {
        if (powerFaded || snrFaded) {
          m_isOpen = false;
        }
      } else if (powerRecovered && snrRecovered) {
        m_isOpen = true;
      }
      return;
    }
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
    if (!m_adaptive && m_openDbfs <= -119.0f) {
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

  // Adaptive fade-mute state.
  bool m_adaptive = false;
  bool m_refInit = false;
  float m_refDbfs = -120.0f;
  float m_dropDb = 10.0f;
  float m_refDecayPerUpdate = 0.0f;
  // Optional secondary fade trigger on the demod-domain SNR.
  bool m_snrRefInit = false;
  float m_snrRef = 0.0f;
};

} // namespace fm_tuner::dsp

#endif
