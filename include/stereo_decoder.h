#ifndef STEREO_DECODER_H
#define STEREO_DECODER_H

#include "dsp/liquid_primitives.h"
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

  bool isStereo() const { return m_stereoDetected; }

private:
  int m_inputRate;
  bool m_stereoDetected;
  bool m_forceStereo;
  bool m_forceMono;
  BlendMode m_blendMode;
  float m_pilotMagnitude;
  float m_pilotBandMagnitude;
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
  fm_tuner::dsp::liquid::NCO m_liquidPilotPll;
};

#endif
