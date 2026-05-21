#include "dsp_pipeline.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <iostream>

namespace {

constexpr size_t kIqStagingBlocks = 4;

} // namespace

// Audio-output soft limiter: passthrough below |x| = kSoftLimitThreshold,
// tanh soft knee above that, hard clamp at ±1.0 as a seatbelt. Threshold is
// at about -1.4 dBFS so stereo-blend transients and resampler overshoot get
// compressed instead of hard-clipped.
float DspPipeline::softLimitSample(float x, uint32_t &softCount) {
  const float ax = std::fabs(x);
  if (ax <= kSoftLimitThreshold) {
    return x;
  }
  ++softCount;
  const float range = 1.0f - kSoftLimitThreshold;
  const float soft =
      kSoftLimitThreshold +
      range * std::tanh((ax - kSoftLimitThreshold) / range);
  return std::clamp((x < 0.0f) ? -soft : soft, -1.0f, 1.0f);
}

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

  m_stereo.setPilotCancellerEnabled(processing.pilot_canceller);

  std::string hicut = processing.hicut;
  std::transform(
      hicut.begin(), hicut.end(), hicut.begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (hicut == "gentle") {
    m_afPost.setHicutMode(AFPostProcessor::HicutMode::Gentle);
  } else if (hicut == "strong") {
    m_afPost.setHicutMode(AFPostProcessor::HicutMode::Strong);
  } else {
    m_afPost.setHicutMode(AFPostProcessor::HicutMode::Off);
  }

  std::string multipath = processing.multipath_eq;
  std::transform(
      multipath.begin(), multipath.end(), multipath.begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  fm_tuner::dsp::MultipathEqMode multipathMode =
      fm_tuner::dsp::MultipathEqMode::Off;
  if (multipath == "light") {
    multipathMode = fm_tuner::dsp::MultipathEqMode::Light;
  } else if (multipath == "aggressive") {
    multipathMode = fm_tuner::dsp::MultipathEqMode::Aggressive;
  }
  m_demod.setMultipathEqMode(
      multipathMode, static_cast<std::uint32_t>(processing.multipath_eq_taps));
  m_demod.setIqFirL1Normalize(processing.iq_fir_l1_normalize);

  // Squelch — operates on the 48 kHz audio output, gated by the per-block
  // channelPowerDbfs estimate from FMDemod. Default config.squelch_dbfs
  // == -120 leaves the gate effectively disabled.
  m_squelch.configure(static_cast<float>(processing.squelch_dbfs), 3.0f,
                      0.030f, m_outputRate);

  const uint32_t decimFactor = static_cast<uint32_t>(m_iqDecimation);
  const uint32_t decimTapsPerPhase =
      (decimFactor >= 8U) ? 28U : ((decimFactor >= 4U) ? 20U : 12U);
  m_iqDecimator.init(decimFactor, decimTapsPerPhase, 80.0f);
  if (m_iqDecimation > 1) {
    const size_t stagingBytes = sdrBlockSamples() * 2 * kIqStagingBlocks;
    m_iqStagingRing.assign(stagingBytes, 0);
    m_iqLinearizedBlock.assign(sdrBlockSamples() * 2, 0);
  }
}

void DspPipeline::reset() {
  m_demod.reset();
  m_stereo.reset();
  m_afPost.reset();
  m_iqDecimator.reset();
  clearIqStaging();
  m_pendingAudioReset = false;
}

void DspPipeline::setBandwidthHz(int bandwidthHz) {
  m_demod.setBandwidthHz(bandwidthHz);
  m_pendingAudioReset = true;
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
  m_pendingAudioReset = true;
}

void DspPipeline::setForceMono(bool forceMono) { m_stereo.setForceMono(forceMono); }

void DspPipeline::clearIqStaging() {
  m_iqStagingReadPos = 0;
  m_iqStagingWritePos = 0;
  m_iqStagingSize = 0;
}

void DspPipeline::appendIqToStaging(const uint8_t *iq, size_t sampleCount) {
  if (!iq || sampleCount == 0 || m_iqStagingRing.empty()) {
    return;
  }
  const size_t incomingBytes = sampleCount * 2;
  const size_t capacity = m_iqStagingRing.size();

  size_t srcOffset = 0;
  if (incomingBytes >= capacity) {
    srcOffset = incomingBytes - capacity;
    clearIqStaging();
  } else if (m_iqStagingSize + incomingBytes > capacity) {
    const size_t drop = (m_iqStagingSize + incomingBytes) - capacity;
    m_iqStagingReadPos = (m_iqStagingReadPos + drop) % capacity;
    m_iqStagingSize -= drop;
  }

  const size_t keptBytes = incomingBytes - srcOffset;
  const uint8_t *src = iq + srcOffset;
  size_t remaining = keptBytes;
  while (remaining > 0) {
    const size_t chunk =
        std::min(remaining, capacity - m_iqStagingWritePos);
    std::memcpy(m_iqStagingRing.data() + m_iqStagingWritePos, src, chunk);
    m_iqStagingWritePos = (m_iqStagingWritePos + chunk) % capacity;
    src += chunk;
    remaining -= chunk;
  }
  m_iqStagingSize += keptBytes;
}

bool DspPipeline::linearizeDecimatorBlock(size_t sampleCount) {
  if (m_iqStagingRing.empty() || m_iqLinearizedBlock.size() < (sampleCount * 2) ||
      m_iqStagingSize < (sampleCount * 2)) {
    return false;
  }

  const size_t requiredBytes = sampleCount * 2;
  const size_t capacity = m_iqStagingRing.size();
  size_t dstOffset = 0;
  size_t remaining = requiredBytes;
  size_t readPos = m_iqStagingReadPos;

  while (remaining > 0) {
    const size_t chunk = std::min(remaining, capacity - readPos);
    std::memcpy(m_iqLinearizedBlock.data() + dstOffset,
                m_iqStagingRing.data() + readPos, chunk);
    readPos = (readPos + chunk) % capacity;
    dstOffset += chunk;
    remaining -= chunk;
  }

  m_iqStagingReadPos = readPos;
  m_iqStagingSize -= requiredBytes;
  return true;
}

bool DspPipeline::process(
    const uint8_t *iq, size_t samples,
    const std::function<void(const float *, size_t)> &rdsSink, Result &out) {
  out = Result{};
  if (!iq || samples == 0) {
    return false;
  }

  if (m_pendingAudioReset) {
    m_stereo.reset();
    m_afPost.reset();
    m_pendingAudioReset = false;
  }

  // Only let the multipath equalizer adapt when we have a strong, locked
  // signal. CMA on a noisy or absent signal will wander to a wrong solution.
  m_demod.setMultipathAdaptEnabled(m_stereo.isStereo());

  const uint8_t *iqForDemod = iq;
  const std::complex<float> *iqForDemodComplex = nullptr;
  size_t demodSamples = samples;

  if (m_iqDecimation > 1) {
    appendIqToStaging(iq, samples);

    if (m_iqStagingSize < (sdrBlockSamples() * 2)) {
      return false;
    }
    if (!linearizeDecimatorBlock(sdrBlockSamples())) {
      return false;
    }

    demodSamples = m_iqDecimator.executeComplex(
        m_iqLinearizedBlock.data(), sdrBlockSamples(),
        m_iqDecimatedComplex.data(),
        m_blockSamples);
    iqForDemodComplex = m_iqDecimatedComplex.data();

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
    m_afPost.setSignalQuality(m_stereo.getStereoQuality());
    outSamples =
        m_afPost.process(m_stereoLeft.data(), m_stereoRight.data(), stereoSamples,
                         m_audioLeft.data(), m_audioRight.data(), m_blockSamples);
    stereoDetected = m_stereo.isStereo();
    pilotTenthsKHz = m_stereo.getPilotLevelTenthsKHz();
  }

  // Squelch — gate decision is per-block (driven by the FMDemod's
  // channel-power estimate), but the per-sample gain ramp inside
  // m_squelch.process() avoids audible clicks at the open/close edge.
  // No-op when squelch_dbfs is at the disable sentinel.
  m_squelch.updateGate(m_demod.getFilteredChannelPowerDbfs());
  m_squelch.process(m_audioLeft.data(), m_audioRight.data(), outSamples);

  uint32_t softClipCount = 0;
  for (size_t i = 0; i < outSamples; i++) {
    m_audioLeft[i] = softLimitSample(m_audioLeft[i], softClipCount);
    m_audioRight[i] = softLimitSample(m_audioRight[i], softClipCount);
  }

  out.left = m_audioLeft.data();
  out.right = m_audioRight.data();
  out.outSamples = outSamples;
  out.demodSamples = demodSamples;
  out.stereoDetected = stereoDetected;
  out.pilotTenthsKHz = pilotTenthsKHz;
  out.stereoBlend = m_stereo.getStereoBlend();
  out.stereoQuality = m_stereo.getStereoQuality();
  out.audioClipRatio =
      (outSamples > 0)
          ? static_cast<float>(softClipCount) /
                static_cast<float>(outSamples * 2U)
          : 0.0f;
  out.channelPowerDbfs = m_demod.getFilteredChannelPowerDbfs();

  // Multipath equalizer telemetry. Only emit when the equalizer is active and
  // adapting, throttled so it doesn't drown out the rest of the log.
  if (m_verboseLogging) {
    const float envErr = m_demod.getMultipathEnvelopeError();
    if (envErr > 0.0f) {
      static uint32_t eqLogCount = 0;
      const uint32_t count = ++eqLogCount;
      if (count <= 5 || (count % 100) == 0) {
        std::cout << "[EQ] env_err=" << envErr << "\n";
      }
    }
  }

  return true;
}
