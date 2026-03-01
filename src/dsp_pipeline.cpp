#include "dsp_pipeline.h"

#include <algorithm>
#include <cctype>
#include <cstring>

DspPipeline::DspPipeline(int inputRate, int outputRate,
                         const Config::ProcessingSection &processing,
                         bool verboseLogging, size_t blockSamples,
                         size_t iqDecimation)
    : m_inputRate(std::max(1, inputRate)),
      m_outputRate(std::max(1, outputRate)),
      m_stereoEnabled(processing.stereo), m_verboseLogging(verboseLogging),
      m_blockSamples(std::max<size_t>(1, blockSamples)),
      m_iqDecimation(std::max<size_t>(1, iqDecimation)),
      m_demod(m_inputRate, m_outputRate), m_stereo(m_inputRate, m_outputRate),
      m_afPost(m_inputRate, m_outputRate), m_iqDecimatedComplex(m_blockSamples),
      m_demodBuffer(m_blockSamples, 0.0f), m_stereoLeft(m_blockSamples, 0.0f),
      m_stereoRight(m_blockSamples, 0.0f), m_audioLeft(m_blockSamples, 0.0f),
      m_audioRight(m_blockSamples, 0.0f) {
  m_demod.setW0BandwidthHz(processing.w0_bandwidth_hz);

  std::string dspAgc = processing.dsp_agc;
  std::transform(
      dspAgc.begin(), dspAgc.end(), dspAgc.begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (dspAgc == "fast") {
    m_demod.setDspAgcMode(FMDemod::DspAgcMode::Fast);
  } else if (dspAgc == "slow") {
    m_demod.setDspAgcMode(FMDemod::DspAgcMode::Slow);
  } else {
    m_demod.setDspAgcMode(FMDemod::DspAgcMode::Off);
  }

  std::string blendMode = processing.stereo_blend;
  std::transform(
      blendMode.begin(), blendMode.end(), blendMode.begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (blendMode == "soft") {
    m_stereo.setBlendMode(StereoDecoder::BlendMode::Soft);
  } else if (blendMode == "aggressive") {
    m_stereo.setBlendMode(StereoDecoder::BlendMode::Aggressive);
  } else {
    m_stereo.setBlendMode(StereoDecoder::BlendMode::Normal);
  }

  const uint32_t decimFactor = static_cast<uint32_t>(m_iqDecimation);
  const uint32_t decimTapsPerPhase =
      (decimFactor >= 8U) ? 28U : ((decimFactor >= 4U) ? 20U : 12U);
  m_iqDecimator.init(decimFactor, decimTapsPerPhase, 80.0f);
  if (m_iqDecimation > 1) {
    m_iqStaging.reserve(sdrBlockSamples() * 4);
  }
}

void DspPipeline::reset() {
  m_demod.reset();
  m_stereo.reset();
  m_afPost.reset();
  m_iqDecimator.reset();
}

void DspPipeline::setBandwidthHz(int bandwidthHz) {
  m_demod.setBandwidthHz(bandwidthHz);
}

void DspPipeline::setDeemphasisMode(int deemphasisMode) {
  if (deemphasisMode == 0) {
    m_afPost.setDeemphasis(50);
    m_demod.setDeemphasis(50);
  } else if (deemphasisMode == 1) {
    m_afPost.setDeemphasis(75);
    m_demod.setDeemphasis(75);
  } else {
    m_afPost.setDeemphasis(0);
    m_demod.setDeemphasis(0);
  }
}

void DspPipeline::setForceMono(bool forceMono) { m_stereo.setForceMono(forceMono); }

bool DspPipeline::process(
    const uint8_t *iq, size_t samples,
    const std::function<void(const float *, size_t)> &rdsSink, Result &out) {
  out = Result{};
  if (!iq || samples == 0) {
    return false;
  }

  const uint8_t *iqForDemod = iq;
  const std::complex<float> *iqForDemodComplex = nullptr;
  size_t demodSamples = samples;

  if (m_iqDecimation > 1) {
    const size_t appendedBytes = samples * 2;
    const size_t oldSize = m_iqStaging.size();
    m_iqStaging.resize(oldSize + appendedBytes);
    std::memcpy(m_iqStaging.data() + oldSize, iq, appendedBytes);

    const size_t availableSamples = m_iqStaging.size() / 2;
    if (availableSamples < sdrBlockSamples()) {
      return false;
    }

    demodSamples = m_iqDecimator.executeComplex(
        m_iqStaging.data(), sdrBlockSamples(), m_iqDecimatedComplex.data(),
        m_blockSamples);
    iqForDemodComplex = m_iqDecimatedComplex.data();

    const size_t consumedBytes = sdrBlockSamples() * 2;
    const size_t remainingBytes = m_iqStaging.size() - consumedBytes;
    if (remainingBytes > 0) {
      std::memmove(m_iqStaging.data(), m_iqStaging.data() + consumedBytes,
                   remainingBytes);
    }
    m_iqStaging.resize(remainingBytes);

    if (demodSamples == 0) {
      return false;
    }
  } else {
    iqForDemod = iq;
  }

  size_t outSamples = 0;
  bool stereoDetected = false;
  int pilotTenthsKHz = 0;

  if (!m_stereoEnabled) {
    outSamples =
        (iqForDemodComplex != nullptr)
            ? m_demod.processSplitComplex(iqForDemodComplex, m_demodBuffer.data(),
                                          m_audioLeft.data(), demodSamples)
            : m_demod.processSplit(iqForDemod, m_demodBuffer.data(),
                                   m_audioLeft.data(), demodSamples);
    if (rdsSink) {
      rdsSink(m_demodBuffer.data(), demodSamples);
    }
    for (size_t i = 0; i < outSamples; i++) {
      const float mono = m_audioLeft[i] * 0.5f;
      m_audioLeft[i] = mono;
      m_audioRight[i] = mono;
    }
  } else {
    if (iqForDemodComplex != nullptr) {
      m_demod.processSplitComplex(iqForDemodComplex, m_demodBuffer.data(),
                                  nullptr, demodSamples);
    } else {
      m_demod.processSplit(iqForDemod, m_demodBuffer.data(), nullptr,
                           demodSamples);
    }
    if (rdsSink) {
      rdsSink(m_demodBuffer.data(), demodSamples);
    }
    const size_t stereoSamples =
        m_stereo.processAudio(m_demodBuffer.data(), m_stereoLeft.data(),
                              m_stereoRight.data(), demodSamples);
    outSamples =
        m_afPost.process(m_stereoLeft.data(), m_stereoRight.data(), stereoSamples,
                         m_audioLeft.data(), m_audioRight.data(), m_blockSamples);
    stereoDetected = m_stereo.isStereo();
    pilotTenthsKHz = m_stereo.getPilotLevelTenthsKHz();
  }

  for (size_t i = 0; i < outSamples; i++) {
    m_audioLeft[i] = std::clamp(m_audioLeft[i], -1.0f, 1.0f);
    m_audioRight[i] = std::clamp(m_audioRight[i], -1.0f, 1.0f);
  }

  out.left = m_audioLeft.data();
  out.right = m_audioRight.data();
  out.outSamples = outSamples;
  out.demodSamples = demodSamples;
  out.stereoDetected = stereoDetected;
  out.pilotTenthsKHz = pilotTenthsKHz;
  return true;
}
