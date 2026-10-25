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
  // is calibrated at init, so the pilot power maps to deviation via the
  // demod's 1.0 == 75 kHz scaling: dev = (amplitude / filterGain) * 75.
  // Nominal pilot is ~6.75 kHz (9% injection). The noise that falls inside
  // the pilot band (measured in the empty guard band beside it) is
  // subtracted, and the reading is 0 while no pilot stands above that noise:
  // without this a dead channel read ~10 kHz of "pilot".
  float getPilotDeviationKHz() const {
    if (m_pilotSnrDb < kPilotSnrReportDb) {
      return 0.0f;
    }
    const float g = (m_pilotFilterGain > 1e-4f) ? m_pilotFilterGain : 0.5f;
    return (std::sqrt(m_pilotSignalMs) / g) * 75.0f;
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
    // The noise inside the RDS band, measured in the empty band above it and
    // scaled by the calibrated bandwidth ratio, is subtracted first: FM noise
    // rises with frequency, and without the correction a weak or dead channel
    // read 15-30 kHz of "RDS".
    const float signalMs =
        std::max(0.0f, m_rdsBandMs - (m_rdsNoiseScale * m_rdsProbeMs));
    return (std::sqrt(signalMs) / g) * 75.0f * kRdsCrestFactor;
  }

  // Pilot-to-noise ratio in dB inside the pilot band, from the pilot power
  // and the noise measured in the guard band beside it. A pilot is a spectral
  // line and noise is not, so this separates a real pilot from noise the PLL
  // happens to track - which coherence alone cannot do.
  float getPilotSnrDb() const { return m_pilotSnrDb; }
  // Smoothed frequency error of the pilot PLL against 19 kHz, in Hz.
  float getPllErrorHz() const {
    const float nominal =
        2.0f * 3.14159265358979f * 19000.0f / static_cast<float>(m_inputRate);
    return std::abs(m_pllFreq - nominal) * static_cast<float>(m_inputRate) /
           (2.0f * 3.14159265358979f);
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
  // Narrow-band power probe at a fixed frequency: the MPX is mixed down by a
  // phase accumulator and low-passed by a 4th-order Butterworth (two
  // biquads). Placed in an empty guard band of the multiplex it measures the
  // demodulator noise density next to a subcarrier, at a few multiplies per
  // sample.
  struct NoiseProbe {
    float phase = 0.0f;
    float step = 0.0f;
    float b0[2] = {1.0f, 1.0f};
    float b1[2] = {0.0f, 0.0f};
    float b2[2] = {0.0f, 0.0f};
    float a1[2] = {0.0f, 0.0f};
    float a2[2] = {0.0f, 0.0f};
    std::complex<float> z1[2];
    std::complex<float> z2[2];
    void init(float centerHz, float cutoffHz, float sampleRate);
    void reset();
    // Returns the probe's output power |y|^2 for one input sample.
    float process(float x);
  };

  // A narrow band of the multiplex taken at a fraction of the input rate.
  // The MPX is mixed down by a phase accumulator so the band sits at DC, then
  // low-passed by a real-tap FIR that is evaluated only every `decim` input
  // samples. With the band-pass's own Kaiser prototype as the low-pass, the
  // output is the band-pass output shifted to DC, sampled at fs/decim: the
  // same filter for 1/decim of the work. (At 256 kHz the pilot and RDS
  // band-passes ran 245 and 163 taps on every sample and were the largest
  // share of the decoder's time.)
  struct BasebandBand {
    fm_tuner::dsp::liquid::FIRFilter lowpass;
    float step = 0.0f;  // mixer phase increment, rad/sample
    float phase = 0.0f; // mixer phase of the next input sample
    int decim = 1;
    int count = 0;      // input samples since the last evaluation
    std::complex<float> prev{0.0f, 0.0f};
    std::complex<float> cur{0.0f, 0.0f};
    void init(const std::vector<float> &taps, float centerHz, float sampleRate,
              int decimation);
    void reset();
    // Mixes and pushes one sample. Returns true when a new low-pass output
    // was computed (once every `decim` samples); it is then in `cur`. The
    // mixer phase used for this sample is returned in `mixPhase`.
    bool push(float x, float &mixPhase);
    // The low-pass output `decim` samples ago, linearly interpolated between
    // the last two evaluations: a continuous per-sample view of the band,
    // delayed by exactly `decim` samples.
    std::complex<float> interpolated() const;
  };

  static constexpr float kPilotSnrReportDb = 3.0f;
  void calibrateNoiseScales();
  void updatePilotSnr();

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
  float m_pilotBandMs = 0.0f;    // EMA of |pilot band|^2
  float m_pilotProbeLowMs = 0.0f;  // EMA of the 17 kHz guard-band noise probe
  float m_pilotProbeHighMs = 0.0f; // EMA of the 21 kHz guard-band noise probe
  float m_rdsProbeMs = 0.0f;     // EMA of the noise probe above the RDS band
  float m_pilotNoiseScaleLow = 1.0f;  // probe power -> noise in pilot band
  float m_pilotNoiseScaleHigh = 1.0f; // probe power -> noise in pilot band
  float m_rdsNoiseScale = 1.0f;   // probe power -> noise power in RDS band
  float m_pilotSignalMs = 0.0f;  // noise-corrected pilot power
  float m_pilotSnrDb = -20.0f;   // pilot-to-noise ratio inside the pilot band
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
  // hard-dropping to mono. Near full blend the filter is crossfaded out: its
  // phase lag acts on L-R only, so left in at full blend it cost most of the
  // high-frequency separation (0.8 dB left at 14 kHz).
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
  // The pilot band: 19 kHz +- 250 Hz, rebuilt per sample from the baseband
  // band for the PLL. m_pilotRotation folds in the prototype's group delay and
  // the interpolation delay, so the rebuilt phasor is the analytic band-pass
  // output m_delaySamples behind the input.
  BasebandBand m_pilotBand;
  std::complex<float> m_pilotRotation{1.0f, 0.0f};
  // The 57 kHz RDS band for the RDS deviation meter. Only its power is used,
  // so it is read at the decimated rate.
  BasebandBand m_rdsBand;
  float m_rdsEnvSmoothDecim = 0.0f; // kPilotEnvSmooth^decim
  fm_tuner::dsp::liquid::IIRFilterReal m_liquidHissBandFilter;
  fm_tuner::dsp::liquid::NCO m_liquidPilotPll;
  NoiseProbe m_pilotNoiseProbeLow;
  NoiseProbe m_pilotNoiseProbeHigh;
  NoiseProbe m_rdsNoiseProbe;
};

#endif
