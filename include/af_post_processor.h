#ifndef AF_POST_PROCESSOR_H
#define AF_POST_PROCESSOR_H

#include "dsp/liquid_primitives.h"
#include <array>
#include <stddef.h>
#include <stdint.h>

class AFPostProcessor {
public:
  enum class HicutMode { Off = 0, Gentle = 1, Strong = 2 };

  AFPostProcessor(int inputRate, int outputRate);

  void reset();
  void setDeemphasis(int tau_us);
  void setHicutMode(HicutMode mode);
  // Quality is 0..1. 1 = clean signal, run only the configured de-emphasis.
  // 0 = degraded, blend toward a narrower (lower-cutoff) de-emphasis so
  // residual hiss from the discriminator is suppressed.
  void setSignalQuality(float quality);

  size_t process(const float *inLeft, const float *inRight, size_t inSamples,
                 float *outLeft, float *outRight, size_t outCapacity);

private:
  void rebuildDeemphasis();

  int m_inputRate;
  int m_outputRate;

  bool m_deemphasisEnabled;
  int m_deemphasisTauUs;
  HicutMode m_hicutMode;
  float m_signalQuality;
  float m_currentHicutWeight; // 0 = normal, 1 = full narrow blend

  // Single-pole DC blocker corner ≈ alpha · Fs / (2π). At Fs = 48 kHz the
  // previous 0.005 produced a ~38 Hz HPF that audibly removed sub-bass on
  // some broadcasts. 0.002 → ~15 Hz, conventional broadcast practice.
  static constexpr float kDcBlockAlpha = 0.002f;
  // EU / ITU-R BS.412 default. US/Korea deployments override via config
  // ([tuner].deemphasis = 1 → 75 µs) or runtime XDR command.
  static constexpr int kDefaultDeemphasisUs = 50;
  static constexpr float kMicrosecondsToSeconds = 1e-6f;
  fm_tuner::dsp::liquid::IIRFilterReal m_liquidLeftDeemphasis;
  fm_tuner::dsp::liquid::IIRFilterReal m_liquidRightDeemphasis;
  fm_tuner::dsp::liquid::IIRFilterReal m_liquidLeftDeemphasisNarrow;
  fm_tuner::dsp::liquid::IIRFilterReal m_liquidRightDeemphasisNarrow;
  fm_tuner::dsp::liquid::IIRFilterReal m_liquidLeftDcBlock;
  fm_tuner::dsp::liquid::IIRFilterReal m_liquidRightDcBlock;
  fm_tuner::dsp::liquid::Resampler m_liquidLeftResampler;
  fm_tuner::dsp::liquid::Resampler m_liquidRightResampler;
  std::array<float, fm_tuner::dsp::liquid::Resampler::kMaxOutput>
      m_liquidLeftTmp{};
  std::array<float, fm_tuner::dsp::liquid::Resampler::kMaxOutput>
      m_liquidRightTmp{};
};

#endif
