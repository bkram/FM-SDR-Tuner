#ifndef DSP_PIPELINE_H
#define DSP_PIPELINE_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "af_post_processor.h"
#include "config.h"
#include "dsp/liquid_primitives.h"
#include "fm_demod.h"
#include "stereo_decoder.h"

class DspPipeline {
public:
  struct Result {
    float *left = nullptr;
    float *right = nullptr;
    size_t outSamples = 0;
    size_t demodSamples = 0;
    bool stereoDetected = false;
    int pilotTenthsKHz = 0;
  };

  DspPipeline(int inputRate, int outputRate,
              const Config::ProcessingSection &processing, bool verboseLogging,
              size_t blockSamples, size_t iqDecimation);

  void reset();
  void setBandwidthHz(int bandwidthHz);
  void setDeemphasisMode(int deemphasisMode);
  void setForceMono(bool forceMono);
  size_t blockSize() const { return m_blockSamples; }
  size_t sdrBlockSamples() const { return m_blockSamples * m_iqDecimation; }

  bool process(const uint8_t *iq, size_t samples,
               const std::function<void(const float *, size_t)> &rdsSink,
               Result &out);

private:
  int m_inputRate;
  int m_outputRate;
  bool m_stereoEnabled;
  bool m_verboseLogging;
  size_t m_blockSamples;
  size_t m_iqDecimation;

  FMDemod m_demod;
  StereoDecoder m_stereo;
  AFPostProcessor m_afPost;
  fm_tuner::dsp::liquid::ComplexDecimator m_iqDecimator;

  std::vector<uint8_t> m_iqStaging;
  std::vector<std::complex<float>> m_iqDecimatedComplex;
  std::vector<float> m_demodBuffer;
  std::vector<float> m_stereoLeft;
  std::vector<float> m_stereoRight;
  std::vector<float> m_audioLeft;
  std::vector<float> m_audioRight;
};

#endif
