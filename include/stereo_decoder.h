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
  // State of the Hi-Blend variable-cutoff one-pole LPF on the L-R signal.
  // Cutoff scales with m_stereoBlend so under weak reception stereo imagery
  // retains LF detail while HF (which carries most of the noise energy) is
  // rolled off, instead of hard-dropping to mono.
  float m_hiBlendLrState;

  std::vector<std::complex<float>> m_delayLine;
  size_t m_delayPos;
  int m_delaySamples;
  fm_tuner::dsp::liquid::FIRFilter m_liquidPilotBandFilter;
  fm_tuner::dsp::liquid::NCO m_liquidPilotPll;
  fm_tuner::dsp::liquid::FIRFilter m_liquidMonoAudioFilter;
  fm_tuner::dsp::liquid::FIRFilter m_liquidLrAudioFilter;
};

#endif
