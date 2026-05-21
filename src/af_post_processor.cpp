#include "af_post_processor.h"

#include <algorithm>
#include <cmath>
#include <vector>

AFPostProcessor::AFPostProcessor(int inputRate, int outputRate)
    : m_inputRate(std::max(1, inputRate)),
      m_outputRate(std::max(1, outputRate)), m_deemphasisEnabled(false),
      m_deemphasisTauUs(0), m_hicutMode(HicutMode::Off), m_signalQuality(1.0f),
      m_currentHicutWeight(0.0f) {
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
    if (m_hicutMode != HicutMode::Off) {
      m_liquidLeftDeemphasisNarrow.reset();
      m_liquidRightDeemphasisNarrow.reset();
    }
  }
  m_currentHicutWeight = 0.0f;
}

void AFPostProcessor::setDeemphasis(int tau_us) {
  if (tau_us <= 0) {
    m_deemphasisEnabled = false;
    m_deemphasisTauUs = 0;
    m_liquidLeftDeemphasis.reset();
    m_liquidRightDeemphasis.reset();
    return;
  }
  m_deemphasisEnabled = true;
  m_deemphasisTauUs = tau_us;
  rebuildDeemphasis();
}

void AFPostProcessor::setHicutMode(HicutMode mode) {
  if (mode == m_hicutMode) {
    return;
  }
  m_hicutMode = mode;
  m_currentHicutWeight = 0.0f;
  if (m_deemphasisEnabled) {
    rebuildDeemphasis();
  }
}

void AFPostProcessor::setSignalQuality(float quality) {
  m_signalQuality = std::clamp(quality, 0.0f, 1.0f);
}

void AFPostProcessor::rebuildDeemphasis() {
  // Bilinear-transformed one-pole de-emphasis, matching GNU Radio
  // gr-analog fm_emph.py. Frequency pre-warping makes the digital response
  // track the analog RC curve more accurately at the 48 kHz output rate.
  const float tau = static_cast<float>(m_deemphasisTauUs) * kMicrosecondsToSeconds;
  const float fs = static_cast<float>(m_outputRate);
  auto designForTau = [&](float tauSec) {
    const float wp = 1.0f / tauSec;
    const float wpp = std::tan(wp / (2.0f * fs));
    const float b0 = wpp / (wpp + 1.0f);
    const float a1 = (wpp - 1.0f) / (wpp + 1.0f);
    return std::pair<std::vector<float>, std::vector<float>>{
        {b0, b0}, {1.0f, a1}};
  };
  auto [coeffsNormal, fbNormal] = designForTau(tau);
  m_liquidLeftDeemphasis.init(coeffsNormal, fbNormal);
  m_liquidRightDeemphasis.init(coeffsNormal, fbNormal);

  if (m_hicutMode != HicutMode::Off) {
    // Narrow-τ design: Gentle = 2×, Strong = 5× the configured τ. Steeper
    // top-end rolloff burying hiss on marginal signals; gentle stays close
    // to spec, strong is the SCA / DXer setting.
    const float multiplier = (m_hicutMode == HicutMode::Gentle) ? 2.0f : 5.0f;
    auto [coeffsNarrow, fbNarrow] = designForTau(tau * multiplier);
    m_liquidLeftDeemphasisNarrow.init(coeffsNarrow, fbNarrow);
    m_liquidRightDeemphasisNarrow.init(coeffsNarrow, fbNarrow);
  }
}

size_t AFPostProcessor::process(const float *inLeft, const float *inRight,
                                size_t inSamples, float *outLeft,
                                float *outRight, size_t outCapacity) {
  if (!inLeft || !inRight || !outLeft || !outRight || inSamples == 0 ||
      outCapacity == 0) {
    return 0;
  }

  // HiCut crossfade target. Lower quality → more narrow blend. (1-q)² gives
  // a gentler ramp than linear; HiCut only opens up meaningfully when quality
  // is below ~0.7. Smoothing alpha at output-rate is ~50 ms time constant so
  // the crossfade tracks stereo quality without ringing.
  const float targetHicutWeight =
      (m_hicutMode == HicutMode::Off)
          ? 0.0f
          : std::clamp((1.0f - m_signalQuality) * (1.0f - m_signalQuality),
                       0.0f, 1.0f);
  const float weightSmoothAlpha =
      1.0f - std::exp(-1.0f / (0.050f * static_cast<float>(m_outputRate)));

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
        const float leftNormal = m_liquidLeftDeemphasis.execute(left);
        const float rightNormal = m_liquidRightDeemphasis.execute(right);
        if (m_hicutMode != HicutMode::Off) {
          m_currentHicutWeight +=
              weightSmoothAlpha * (targetHicutWeight - m_currentHicutWeight);
          const float leftNarrow =
              m_liquidLeftDeemphasisNarrow.execute(left);
          const float rightNarrow =
              m_liquidRightDeemphasisNarrow.execute(right);
          const float w = m_currentHicutWeight;
          left = leftNormal * (1.0f - w) + leftNarrow * w;
          right = rightNormal * (1.0f - w) + rightNarrow * w;
        } else {
          left = leftNormal;
          right = rightNormal;
        }
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
