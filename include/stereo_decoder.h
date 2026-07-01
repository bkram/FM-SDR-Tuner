#ifndef STEREO_DECODER_H
#define STEREO_DECODER_H

#include "dsp/liquid_primitives.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <stddef.h>
#include <stdint.h>
#include <vector>

class StereoDecoder {
public:
  enum class BlendMode { Soft = 0, Normal = 1, Aggressive = 2 };

  StereoDecoder(int inputRate, int outputRate);
  ~StereoDecoder();

  size_t processAudio(const float *mono, float *left, float *right,
                      size_t numSamples);
  void reset();
  void setForceStereo(bool force);
  void setForceMono(bool force);
  void setBlendMode(BlendMode mode) { m_blendMode = mode; }
  void setPilotCancellerEnabled(bool enabled) {
    m_pilotCancellerEnabled = enabled;
  }
  int getPilotLevelTenthsKHz() const { return m_pilotLevelTenthsKHz; }
  float getStereoBlend() const { return m_stereoBlend; }
  float getStereoQuality() const { return m_stereoQuality; }
  // Smoothed total MPX magnitude (composite baseband out of the discriminator);
  // a relative measure of total FM deviation / MPX power.
  float getMpxMagnitude() const { return m_mpxMagnitude; }
  // True 19 kHz pilot peak deviation in kHz. The pilot band-pass gain at 19 kHz
  // is calibrated at init (the centered FIR is L1-normalized, so sub-unity), so
  // the smoothed pilot magnitude maps to deviation via the demod's
  // 1.0 == 75 kHz scaling: dev = (mag / filterGain) * 75. Nominal pilot is
  // ~6.75 kHz (9% injection).
  float getPilotDeviationKHz() const {
    const float g = (m_pilotFilterGain > 1e-4f) ? m_pilotFilterGain : 0.5f;
    // Use the scalar-smoothed band magnitude (not the vector-smoothed coherent
    // m_pilotMagnitude, which shrinks under PLL phase jitter and under-reads
    // the deviation). This matches the scalar |filter output| calibration.
    return (m_pilotBandMagnitude / g) * 75.0f;
  }
  // 57 kHz RDS subcarrier deviation in kHz (calibrated via the band-pass gain at
  // init, same as the pilot). RDS is DSB-SC, so its envelope varies with the
  // data; we report the RMS of the envelope (a stable, principled measure of
  // the subcarrier magnitude — for the pure-tone pilot RMS==mean, so that path
  // is unaffected). Nominal RDS injection is ~2-4 kHz.
  float getRdsDeviationKHz() const {
    const float g = (m_rdsFilterGain > 1e-4f) ? m_rdsFilterGain : 0.5f;
    // RMS of the band envelope -> peak deviation via the RDS biphase crest
    // factor. The 1.64 constant was validated against a reference FM monitor on
    // two stations (RMS readings of 2.2/2.6 kHz vs reported 3.6/4.3 kHz, ratio
    // 1.64/1.65). The pilot path is a pure tone (crest 1) and is unaffected.
    constexpr float kRdsCrestFactor = 1.64f;
    return (std::sqrt(m_rdsBandMs) / g) * 75.0f * kRdsCrestFactor;
  }

  // Demod-domain SNR/quality in dB, from the ratio of the in-band composite
  // power to the noise-triangle hiss power (a signal-free MPX band above the
  // subcarriers). Always available (time-domain, per block), unlike the
  // FFT-based signal-meter SNR which is NaN when no channel-FFT estimate runs.
  // This is a reception-quality figure, not a calibrated absolute. Returns 0
  // until the estimator has settled or when the channel is too narrow to
  // contain the hiss band.
  float getDemodSnrDb() const {
    const float noiseMs = (m_hissNoiseMs > 1e-12f) ? m_hissNoiseMs : 1e-12f;
    // m_mpxMagnitude is a smoothed mean-|mpx|; square it for an in-band power
    // proxy. Both sides share the same smoothing, so the ratio is consistent.
    const float sigMs = m_mpxMagnitude * m_mpxMagnitude;
    const float ratio = sigMs / noiseMs;
    if (!(ratio > 0.0f)) {
      return 0.0f;
    }
    const float snrDb = 10.0f * std::log10(ratio);
    return std::clamp(snrDb, 0.0f, 60.0f);
  }

  bool isStereo() const { return m_stereoDetected; }

private:
  int m_inputRate;
  bool m_stereoDetected;
  bool m_forceStereo;
  bool m_forceMono;
  BlendMode m_blendMode;
  float m_pilotMagnitude;
  float m_pilotBandMagnitude;
  float m_pilotFilterGain = 0.5f; // pilot band-pass gain at 19 kHz (calibrated)
  float m_rdsBandMs = 0.0f;      // EMA of |57 kHz RDS band| envelope, squared
  float m_rdsFilterGain = 0.5f;  // RDS band-pass gain at 57 kHz (calibrated)
  float m_hissNoiseMs = 0.0f;    // EMA of the noise-triangle hiss band, squared
  float m_pilotResidualMagnitude;
  float m_mpxMagnitude;
  float m_stereoBlend;
  float m_stereoQuality;
  int m_pilotLevelTenthsKHz;
  float m_pilotI;
  float m_pilotQ;
  float m_lrRawMagnitude;
  float m_lrAudioMagnitude;

  float m_pllPhase;
  float m_pllFreq;
  float m_pllMinFreq;
  float m_pllMaxFreq;
  float m_pilotConfidence;
  // Hi-Blend LPF on the L-R signal. 2nd-order Butterworth biquad, Direct Form
  // II Transposed. Cutoff scales with m_stereoBlend² so under weak reception
  // stereo imagery retains LF detail while HF (which carries most of the
  // stereo-decoded noise) is rolled off cleanly at 12 dB/oct rather than
  // hard-dropping to mono. Coefficients are recomputed per block from the
  // current blend; intra-block parameter changes are inaudible at audio block
  // rates.
  float m_hiBlendZ1;
  float m_hiBlendZ2;
  float m_hiBlendB0;
  float m_hiBlendB1;
  float m_hiBlendB2;
  float m_hiBlendA1;
  float m_hiBlendA2;
  // 19 kHz pilot canceller: subtracts gI·cos(pll) + gQ·sin(pll) from the mono
  // path. Two-tap LMS so any pilot phase in the input cancels cleanly even
  // with the one-sample PLL/L-R timing offset. Updates only while we believe
  // the pilot is real (locked or force-stereo) to avoid chasing noise.
  float m_pilotCancelGainI;
  float m_pilotCancelGainQ;
  bool m_pilotCancellerEnabled;

  std::vector<std::complex<float>> m_delayLine;
  size_t m_delayPos;
  int m_delaySamples;
  fm_tuner::dsp::liquid::FIRFilter m_liquidPilotBandFilter;
  fm_tuner::dsp::liquid::FIRFilter m_liquidRdsBandFilter;
  fm_tuner::dsp::liquid::IIRFilterReal m_liquidHissBandFilter;
  fm_tuner::dsp::liquid::NCO m_liquidPilotPll;
};

#endif
