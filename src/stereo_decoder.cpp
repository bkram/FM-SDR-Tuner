#include "stereo_decoder.h"
#include <algorithm>

namespace {
constexpr float kPi = 3.14159265358979323846f;
constexpr int kPilotAcquireBlocks = 3;
constexpr int kPilotLossBlocks = 24;
constexpr float kPilotAbsAcquire = 0.0018f;
constexpr float kPilotAbsHold = 0.0011f;
constexpr float kPilotRatioAcquire = 0.040f;
constexpr float kPilotRatioHold = 0.022f;
constexpr float kMpxMinAcquire = 0.005f;
constexpr float kMpxMinHold = 0.0028f;
constexpr float kPilotCoherenceAcquire = 0.18f;
constexpr float kPilotCoherenceHold = 0.11f;
constexpr float kPllLockAcquireHz = 180.0f;
constexpr float kPllLockHoldHz = 320.0f;
constexpr float kPilotRatioQualityFloor = 0.080f;
constexpr float kPilotRatioQualityCeil = 0.110f;
constexpr float kPilotCoherenceQualityFloor = 0.55f;
constexpr float kPilotCoherenceQualityCeil = 0.70f;
constexpr float kPllQualityBestHz = 40.0f;
constexpr float kPllQualityWorstHz = 700.0f;
constexpr float kPilotEnvSmooth = 0.9995f;
constexpr float kPilotEnvInject = 1.0f - kPilotEnvSmooth;
constexpr float kPilotIqSmooth = 0.9995f;
constexpr float kPilotIqInject = 1.0f - kPilotIqSmooth;
} // namespace

StereoDecoder::StereoDecoder(int inputRate, int /*outputRate*/)
    : m_inputRate(inputRate), m_stereoDetected(false), m_forceStereo(false),
      m_forceMono(false), m_blendMode(BlendMode::Normal),
      m_pilotMagnitude(0.0f), m_pilotBandMagnitude(0.0f),
      m_pilotResidualMagnitude(0.0f), m_mpxMagnitude(0.0f),
      m_stereoBlend(0.0f), m_stereoQuality(0.0f), m_pilotLevelTenthsKHz(0),
      m_pilotI(0.0f), m_pilotQ(0.0f), m_lrRawMagnitude(0.0f),
      m_lrAudioMagnitude(0.0f), m_pllPhase(0.0f),
      m_pllFreq(2.0f * kPi * 19000.0f / static_cast<float>(inputRate)),
      m_pllMinFreq(2.0f * kPi * 18750.0f / static_cast<float>(inputRate)),
      m_pllMaxFreq(2.0f * kPi * 19250.0f / static_cast<float>(inputRate)),
      m_pilotCount(0), m_pilotLossCount(0), m_delayPos(0), m_delaySamples(0) {
  constexpr float pilotCenterHz = 19000.0f;
  constexpr float pilotHalfBandwidthHz = 250.0f;
  constexpr float pilotTransitionHz = 3000.0f;
  int pilotTapCount =
      static_cast<int>(std::ceil(3.8 * static_cast<double>(m_inputRate) /
                                 static_cast<double>(pilotTransitionHz)));
  pilotTapCount = std::clamp(pilotTapCount, 63, 511);
  if ((pilotTapCount % 2) == 0) {
    pilotTapCount++;
  }
  const float pilotCenterNorm = std::clamp(
      pilotCenterHz / static_cast<float>(m_inputRate), 0.001f, 0.49f);
  const float pilotCutoffNorm = std::clamp(
      pilotHalfBandwidthHz / static_cast<float>(m_inputRate), 0.0005f, 0.45f);
  m_liquidPilotBandFilter.init(static_cast<std::uint32_t>(pilotTapCount),
                               pilotCutoffNorm, 60.0f, pilotCenterNorm);
  const float audioCutoffNorm =
      std::clamp(15000.0f / static_cast<float>(m_inputRate), 0.01f, 0.45f);
  m_liquidMonoAudioFilter.init(121, audioCutoffNorm);
  m_liquidLrAudioFilter.init(121, audioCutoffNorm);

  m_delaySamples = std::max(0, (pilotTapCount - 1) / 2);
  m_delayLine.assign(static_cast<size_t>(std::max(1, m_delaySamples + 1)),
                     0.0f);
  const float nominalPllFreq =
      2.0f * kPi * 19000.0f / static_cast<float>(m_inputRate);
  m_liquidPilotPll.init(LIQUID_VCO, nominalPllFreq);
  m_liquidPilotPll.setPLLBandwidth(0.01f);
}

StereoDecoder::~StereoDecoder() = default;

void StereoDecoder::reset() {
  m_stereoDetected = false;
  m_pilotMagnitude = 0.0f;
  m_pilotBandMagnitude = 0.0f;
  m_pilotResidualMagnitude = 0.0f;
  m_mpxMagnitude = 0.0f;
  m_stereoBlend = 0.0f;
  m_stereoQuality = 0.0f;
  m_pilotLevelTenthsKHz = 0;
  m_pilotI = 0.0f;
  m_pilotQ = 0.0f;
  m_lrRawMagnitude = 0.0f;
  m_lrAudioMagnitude = 0.0f;
  m_pllPhase = 0.0f;
  m_pllFreq = 2.0f * kPi * 19000.0f / static_cast<float>(m_inputRate);
  m_pilotCount = 0;
  m_pilotLossCount = 0;
  m_delayPos = 0;
  std::fill(m_delayLine.begin(), m_delayLine.end(), 0.0f);
  m_liquidPilotBandFilter.reset();
  m_liquidPilotPll.reset();
  m_liquidMonoAudioFilter.reset();
  m_liquidLrAudioFilter.reset();
}

void StereoDecoder::setForceStereo(bool force) { m_forceStereo = force; }

void StereoDecoder::setForceMono(bool force) { m_forceMono = force; }

size_t StereoDecoder::processAudio(const float *mono, float *left, float *right,
                                   size_t numSamples) {
  if (!mono || !left || !right || numSamples == 0) {
    return 0;
  }

  float attackTau = 0.045f;
  float releaseTau = 0.018f;
  float lowQualityGate = 0.70f;
  float lockFloor = 0.85f;
  if (m_blendMode == BlendMode::Soft) {
    attackTau = 0.035f;
    releaseTau = 0.024f;
    lowQualityGate = 0.62f;
    lockFloor = 0.92f;
  } else if (m_blendMode == BlendMode::Aggressive) {
    attackTau = 0.080f;
    releaseTau = 0.012f;
    lowQualityGate = 0.82f;
    lockFloor = 0.65f;
  }

  const float blendAttack =
      1.0f - std::exp(-1.0f / (attackTau * static_cast<float>(m_inputRate)));
  const float blendRelease =
      1.0f - std::exp(-1.0f / (releaseTau * static_cast<float>(m_inputRate)));
  const float nominalPllFreq =
      2.0f * kPi * 19000.0f / static_cast<float>(m_inputRate);
  auto computeBlendTarget = [&](float pilotMag, float pilotRatio,
                                float pilotCoherence, float pilotResidualRatio,
                                float pllErrHz) -> float {
    if (m_forceMono) {
      m_stereoQuality = 0.0f;
      return 0.0f;
    }
    if (m_forceStereo) {
      m_stereoQuality = 1.0f;
      return 1.0f;
    }

    const float ratioQ =
        std::clamp((pilotRatio - kPilotRatioQualityFloor) /
                       std::max(kPilotRatioQualityCeil - kPilotRatioQualityFloor,
                                1e-4f),
                   0.0f, 1.0f);
    const float cohQ = std::clamp(
        (pilotCoherence - kPilotCoherenceQualityFloor) /
            std::max(kPilotCoherenceQualityCeil - kPilotCoherenceQualityFloor,
                     1e-4f),
        0.0f, 1.0f);
    const float cleanQ =
        std::clamp((1.10f - pilotResidualRatio) / 0.40f, 0.0f, 1.0f);
    const float pllQ =
        std::clamp((kPllQualityWorstHz - pllErrHz) /
                       std::max(kPllQualityWorstHz - kPllQualityBestHz, 1e-3f),
                   0.0f, 1.0f);
    (void)pilotMag;
    const float quality =
        std::pow(std::max(0.0f, ratioQ * cohQ * cleanQ * pllQ), 0.25f);
    m_stereoQuality = quality;
    float qualityShaped = quality;
    if (m_blendMode == BlendMode::Soft) {
      qualityShaped = std::min(1.0f, 0.10f + (0.90f * std::sqrt(std::max(0.0f, quality))));
    } else if (m_blendMode == BlendMode::Normal) {
      qualityShaped = std::min(1.0f, 0.08f + (0.92f * quality));
    } else if (m_blendMode == BlendMode::Aggressive) {
      qualityShaped = quality * quality * quality;
    }

    // Drop to mono quickly when pilot quality degrades to avoid noisy/choppy
    // stereo.
    if (pilotRatio < (kPilotRatioHold * lowQualityGate) ||
        pilotCoherence < (kPilotCoherenceHold * lowQualityGate) ||
        pllErrHz > (kPllLockHoldHz * 1.25f)) {
      return 0.0f;
    }

    if (m_stereoDetected) {
      const float floorKeep =
          std::clamp((quality - 0.50f) / 0.20f, 0.0f, 1.0f);
      const float adaptiveFloor = lockFloor * floorKeep;
      return std::clamp(adaptiveFloor + ((1.0f - adaptiveFloor) * qualityShaped),
                        0.0f, 1.0f);
    }

    // Keep output strictly mono until lock is confirmed to avoid noisy
    // pseudo-stereo.
    return 0.0f;
  };

  size_t outCount = 0;
  for (size_t i = 0; i < numSamples; i++) {
    const float mpx = mono[i];

    m_liquidPilotBandFilter.push(std::complex<float>(mpx, 0.0f));
    const float pilot = m_liquidPilotBandFilter.execute().real();
    m_pilotBandMagnitude = (m_pilotBandMagnitude * kPilotEnvSmooth) +
                           (std::abs(pilot) * kPilotEnvInject);
    m_mpxMagnitude =
        (m_mpxMagnitude * kPilotEnvSmooth) + (std::abs(mpx) * kPilotEnvInject);
    const float phaseNow = m_liquidPilotPll.phase();
    const float vcoI = std::cos(phaseNow);
    const float vcoQ = std::sin(phaseNow);
    const float error = pilot * vcoQ;
    m_liquidPilotPll.stepPLL(error);
    m_liquidPilotPll.step();
    const float phaseNext = m_liquidPilotPll.phase();
    float dphi = phaseNext - phaseNow;
    if (dphi > kPi) {
      dphi -= 2.0f * kPi;
    } else if (dphi < -kPi) {
      dphi += 2.0f * kPi;
    }
    m_pllPhase = phaseNext;
    m_pllFreq = std::clamp(dphi, m_pllMinFreq, m_pllMaxFreq);

    m_pilotI = (m_pilotI * kPilotIqSmooth) + ((pilot * vcoI) * kPilotIqInject);
    m_pilotQ = (m_pilotQ * kPilotIqSmooth) + ((pilot * vcoQ) * kPilotIqInject);
    const float pilotMagNow =
        std::sqrt((m_pilotI * m_pilotI) + (m_pilotQ * m_pilotQ));
    const float pilotRatioNow =
        m_pilotBandMagnitude / std::max(m_mpxMagnitude, 1e-3f);
    const float pilotCoherenceNow =
        pilotMagNow / std::max(m_pilotBandMagnitude, 1e-4f);
    const float pilotReconstructed = (m_pilotI * vcoI) + (m_pilotQ * vcoQ);
    const float pilotResidual = pilot - pilotReconstructed;
    m_pilotResidualMagnitude =
        (m_pilotResidualMagnitude * kPilotEnvSmooth) +
        (std::abs(pilotResidual) * kPilotEnvInject);
    const float pilotResidualRatioNow =
        m_pilotResidualMagnitude / std::max(pilotMagNow, 1e-4f);
    const float pllErrHzNow = std::abs(m_pllFreq - nominalPllFreq) *
                              static_cast<float>(m_inputRate) / (2.0f * kPi);

    const float delayedMpx = m_delayLine[m_delayPos];
    m_delayLine[m_delayPos] = mpx;
    m_delayPos++;
    if (m_delayPos >= m_delayLine.size()) {
      m_delayPos = 0;
    }

    // SDR++-style L-R recovery:
    // 1) take delayed MPX as complex (real, imag=0),
    // 2) multiply by conjugated PLL output twice (38 kHz downconversion),
    // 3) take real part and apply 2x gain.
    const float pllRe = std::cos(m_pllPhase);
    const float pllIm = std::sin(m_pllPhase);
    const float cos2 = (pllRe * pllRe) - (pllIm * pllIm);
    const float monoRaw = delayedMpx;
    const float lrRaw = 2.0f * delayedMpx * cos2;
    m_lrRawMagnitude =
        (m_lrRawMagnitude * kPilotEnvSmooth) + (std::abs(lrRaw) * kPilotEnvInject);
    const float targetStereoBlend = computeBlendTarget(
        pilotMagNow, pilotRatioNow, pilotCoherenceNow, pilotResidualRatioNow,
        pllErrHzNow);

    float blendAlpha =
        (targetStereoBlend > m_stereoBlend) ? blendAttack : blendRelease;
    if (targetStereoBlend < m_stereoBlend) {
      const float qualityDrop = std::clamp((0.70f - m_stereoQuality) / 0.25f,
                                           0.0f, 1.0f);
      blendAlpha = std::min(1.0f, blendAlpha * (1.0f + (2.2f * qualityDrop)));
    }
    m_stereoBlend += (targetStereoBlend - m_stereoBlend) * blendAlpha;

    const float leftRaw = 0.5f * (monoRaw + (lrRaw * m_stereoBlend));
    const float rightRaw = 0.5f * (monoRaw - (lrRaw * m_stereoBlend));
    m_liquidMonoAudioFilter.push(std::complex<float>(leftRaw, 0.0f));
    m_liquidLrAudioFilter.push(std::complex<float>(rightRaw, 0.0f));
    const float leftFilt = m_liquidMonoAudioFilter.execute().real();
    const float rightFilt = m_liquidLrAudioFilter.execute().real();
    m_lrAudioMagnitude =
        (m_lrAudioMagnitude * kPilotEnvSmooth) +
        (0.5f * (std::abs(leftFilt - rightFilt)) * kPilotEnvInject);

    left[outCount] = leftFilt;
    right[outCount] = rightFilt;
    outCount++;
  }

  const float pilotMag =
      std::sqrt((m_pilotI * m_pilotI) + (m_pilotQ * m_pilotQ));
  m_pilotMagnitude = pilotMag;

  const float mpxThreshold = m_stereoDetected ? kMpxMinHold : kMpxMinAcquire;
  const float qualityThreshold = m_stereoDetected ? 0.48f : 0.58f;
  const float pllErrHz = std::abs(m_pllFreq - nominalPllFreq) *
                         static_cast<float>(m_inputRate) / (2.0f * kPi);
  const float pllThreshold =
      m_stereoDetected ? kPllLockHoldHz : kPllLockAcquireHz;
  const bool pilotPresent =
      (m_mpxMagnitude > mpxThreshold) &&
      (m_stereoQuality > qualityThreshold) &&
      (pllErrHz < (pllThreshold * 1.15f));
  if (!m_forceStereo) {
    if (!m_stereoDetected) {
      if (pilotPresent) {
        m_pilotCount++;
        m_pilotLossCount = 0;
        if (m_pilotCount >= kPilotAcquireBlocks) {
          m_stereoDetected = true;
        }
      } else {
        m_pilotCount = std::max(0, m_pilotCount - 1);
      }
    } else if (pilotPresent) {
      m_pilotLossCount = 0;
    } else if (++m_pilotLossCount >= kPilotLossBlocks) {
      m_stereoDetected = false;
      m_pilotCount = 0;
      m_pilotLossCount = 0;
    }
  }

  const float calibrated = m_pilotMagnitude * 8.0f;
  m_pilotLevelTenthsKHz =
      std::clamp(static_cast<int>(std::round(calibrated * 750.0f)), 0, 750);
  return outCount;
}
