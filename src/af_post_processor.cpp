#include "af_post_processor.h"

#include <algorithm>
#include <cmath>
#include <vector>

AFPostProcessor::AFPostProcessor(int inputRate, int outputRate)
    : m_inputRate(std::max(1, inputRate)),
      m_outputRate(std::max(1, outputRate)), m_deemphasisEnabled(false) {
  const float ratio =
      static_cast<float>(m_outputRate) / static_cast<float>(m_inputRate);
  m_liquidLeftResampler.init(ratio);
  m_liquidRightResampler.init(ratio);
  m_liquidLeftDcBlock.initDCBlocker(kDcBlockAlpha);
  m_liquidRightDcBlock.initDCBlocker(kDcBlockAlpha);
  reset();
  setDeemphasis(kDefaultDeemphasisUs);
}

void AFPostProcessor::reset() {
  m_liquidLeftResampler.reset();
  m_liquidRightResampler.reset();
  m_liquidLeftDcBlock.reset();
  m_liquidRightDcBlock.reset();
  if (m_deemphasisEnabled) {
    m_liquidLeftDeemphasis.reset();
    m_liquidRightDeemphasis.reset();
  }
}

void AFPostProcessor::setDeemphasis(int tau_us) {
  if (tau_us <= 0) {
    m_deemphasisEnabled = false;
    m_liquidLeftDeemphasis.reset();
    m_liquidRightDeemphasis.reset();
    return;
  }

  m_deemphasisEnabled = true;
  // Bilinear-transformed one-pole de-emphasis, matching GNU Radio
  // gr-analog fm_emph.py. Frequency pre-warping makes the digital response
  // track the analog RC curve more accurately at the 48 kHz output rate
  // (previous naïve form drifted ~0.5 dB near 10 kHz).
  const float tau = static_cast<float>(tau_us) * kMicrosecondsToSeconds;
  const float fs = static_cast<float>(m_outputRate);
  const float wp = 1.0f / tau;
  const float wpp = std::tan(wp / (2.0f * fs));
  const float b0 = wpp / (wpp + 1.0f);
  const float a1 = (wpp - 1.0f) / (wpp + 1.0f);
  const std::vector<float> coeffs = {b0, b0};
  const std::vector<float> feedback = {1.0f, a1};
  m_liquidLeftDeemphasis.init(coeffs, feedback);
  m_liquidRightDeemphasis.init(coeffs, feedback);
}

size_t AFPostProcessor::process(const float *inLeft, const float *inRight,
                                size_t inSamples, float *outLeft,
                                float *outRight, size_t outCapacity) {
  if (!inLeft || !inRight || !outLeft || !outRight || inSamples == 0 ||
      outCapacity == 0) {
    return 0;
  }

  size_t outCount = 0;
  for (size_t i = 0; i < inSamples && outCount < outCapacity; i++) {
    const uint32_t leftProduced =
        m_liquidLeftResampler.execute(inLeft[i], m_liquidLeftTmp);
    const uint32_t rightProduced =
        m_liquidRightResampler.execute(inRight[i], m_liquidRightTmp);
    const uint32_t produced = std::min(leftProduced, rightProduced);

    for (uint32_t idx = 0; idx < produced && outCount < outCapacity; idx++) {
      float left = m_liquidLeftTmp[idx];
      float right = m_liquidRightTmp[idx];
      if (m_deemphasisEnabled) {
        left = m_liquidLeftDeemphasis.execute(left);
        right = m_liquidRightDeemphasis.execute(right);
      }
      left = m_liquidLeftDcBlock.execute(left);
      right = m_liquidRightDcBlock.execute(right);
      outLeft[outCount] = left;
      outRight[outCount] = right;
      outCount++;
    }
  }
  return outCount;
}
