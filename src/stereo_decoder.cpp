#include "stereo_decoder.h"
#include <algorithm>
#include <complex>

namespace {
constexpr float kPi = 3.14159265358979323846f;

// Stereo acquire/drop hysteresis operates on a wall-clock-time EMA of pilot
// presence so behavior is independent of block size. Tune these by time,
// not by block count. Timing roughly matches the prior block-counter design
// (3 acquire blocks × 16 ms ≈ 48 ms; 24 drop blocks × 16 ms ≈ 400 ms at the
// previously-assumed 4096-sample block) but no longer depends on block size.
constexpr float kPilotAcquireTauSec = 0.030f;
constexpr float kPilotReleaseTauSec = 0.25f;
constexpr float kPilotAcquireConfidence = 0.80f;
constexpr float kPilotHoldConfidence = 0.20f;

constexpr float kPilotRatioAcquire = 0.040f;
constexpr float kPilotRatioHold = 0.022f;
constexpr float kMpxMinAcquire = 0.005f;
constexpr float kMpxMinHold = 0.0028f;
constexpr float kPilotCoherenceAcquire = 0.18f;
constexpr float kPilotCoherenceHold = 0.11f;
constexpr float kPilotResidualAcquire = 0.95f;
constexpr float kPilotResidualHold = 1.05f;
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
// Gear-shifted pilot PLL bandwidth. The acquire value matches the historical
// pre-gear-shift fixed loop bandwidth (0.01) — it preserves weak-signal lock
// sensitivity that wider acquire bandwidths sacrifice for slightly faster
// retune lock. The hold value (0.003) is the actual improvement: once we're
// locked, narrowing the loop reduces pilot jitter and improves stereo
// separation on strong signals. Both values are in liquid_dsp's normalized
// loop-bandwidth units.
constexpr float kPilotPllBwAcquire = 0.01f;
constexpr float kPilotPllBwHold = 0.003f;
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
      m_pilotConfidence(0.0f), m_hiBlendZ1(0.0f), m_hiBlendZ2(0.0f),
      m_hiBlendB0(1.0f), m_hiBlendB1(0.0f), m_hiBlendB2(0.0f),
      m_hiBlendA1(0.0f), m_hiBlendA2(0.0f),
      m_pilotCancelGainI(0.0f), m_pilotCancelGainQ(0.0f),
      m_pilotCancellerEnabled(true), m_delayPos(0), m_delaySamples(0) {
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
  // No dedicated audio LPF at the input rate. The 19 kHz pilot / 38 kHz
  // L-R subcarrier / 57 kHz RDS subcarrier are all rejected by the
  // anti-aliasing filter in AFPostProcessor's resampler; the residual
  // pilot leakage is mopped up by the LMS pilot canceller above.

  m_delaySamples = std::max(0, (pilotTapCount - 1) / 2);
  m_delayLine.assign(static_cast<size_t>(std::max(1, m_delaySamples + 1)),
                     std::complex<float>(0.0f, 0.0f));
  const float nominalPllFreq =
      2.0f * kPi * 19000.0f / static_cast<float>(m_inputRate);
  m_liquidPilotPll.init(LIQUID_VCO, nominalPllFreq);
  m_liquidPilotPll.setPLLBandwidth(kPilotPllBwAcquire);
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
  m_pilotConfidence = 0.0f;
  m_hiBlendZ1 = 0.0f;
  m_hiBlendZ2 = 0.0f;
  m_pilotCancelGainI = 0.0f;
  m_pilotCancelGainQ = 0.0f;
  m_delayPos = 0;
  std::fill(m_delayLine.begin(), m_delayLine.end(),
            std::complex<float>(0.0f, 0.0f));
  m_liquidPilotBandFilter.reset();
  m_liquidPilotPll.reset();
  m_liquidPilotPll.setPLLBandwidth(kPilotPllBwAcquire);
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
  float lockFloor = 0.78f;
  if (m_blendMode == BlendMode::Soft) {
    attackTau = 0.035f;
    releaseTau = 0.024f;
    lowQualityGate = 0.62f;
    lockFloor = 0.86f;
  } else if (m_blendMode == BlendMode::Aggressive) {
    attackTau = 0.080f;
    releaseTau = 0.012f;
    lowQualityGate = 0.82f;
    lockFloor = 0.58f;
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
        std::pow(std::max(0.0f, ratioQ * cohQ * cleanQ * pllQ), 0.40f);
    m_stereoQuality = quality;
    float qualityShaped = quality;
    if (m_blendMode == BlendMode::Soft) {
      qualityShaped = std::min(1.0f, 0.10f + (0.90f * std::sqrt(std::max(0.0f, quality))));
    } else if (m_blendMode == BlendMode::Normal) {
      qualityShaped = std::min(1.0f, 0.08f + (0.92f * quality));
    } else if (m_blendMode == BlendMode::Aggressive) {
      qualityShaped = quality * quality * quality;
    }

    // Continuous gate: each metric contributes a 0..1 derate factor, multiplied
    // into the final blend target. Replaces the previous hard short-circuit
    // that snapped the target to 0 when any one metric crossed its hold
    // threshold, which was the audible source of "stereo pops" on marginal
    // reception. The product of the three factors goes to 0 smoothly when all
    // three metrics degrade together.
    const float ratioLowLimit = kPilotRatioHold * lowQualityGate;
    const float ratioDerate = std::clamp(
        (pilotRatio - ratioLowLimit) /
            std::max(kPilotRatioHold - ratioLowLimit, 1e-4f),
        0.0f, 1.0f);
    const float coherenceLowLimit = kPilotCoherenceHold * lowQualityGate;
    const float coherenceDerate = std::clamp(
        (pilotCoherence - coherenceLowLimit) /
            std::max(kPilotCoherenceHold - coherenceLowLimit, 1e-4f),
        0.0f, 1.0f);
    const float pllHighLimit = kPllLockHoldHz * 1.25f;
    const float pllDerate = std::clamp(
        (pllHighLimit - pllErrHz) /
            std::max(pllHighLimit - kPllLockHoldHz, 1.0f),
        0.0f, 1.0f);
    const float gateDerate = ratioDerate * coherenceDerate * pllDerate;

    if (m_stereoDetected) {
      const float floorKeep =
          std::clamp((quality - 0.72f) / 0.18f, 0.0f, 1.0f);
      const float adaptiveFloor = lockFloor * floorKeep;
      const float target = std::clamp(
          adaptiveFloor + ((1.0f - adaptiveFloor) * qualityShaped), 0.0f, 1.0f);
      return target * gateDerate;
    }

    // Keep output strictly mono until lock is confirmed to avoid noisy
    // pseudo-stereo.
    return 0.0f;
  };

  constexpr float kButterQ = 0.7071067811865476f; // 1/√2, Butterworth Q
  const float fs = static_cast<float>(m_inputRate);
  const float kPiOverFs = kPi / fs;

  size_t outCount = 0;
  for (size_t i = 0; i < numSamples; i++) {
    const float mpx = mono[i];

    m_liquidPilotBandFilter.push(std::complex<float>(mpx, 0.0f));
    const std::complex<float> pilot = m_liquidPilotBandFilter.execute();
    m_pilotBandMagnitude = (m_pilotBandMagnitude * kPilotEnvSmooth) +
                           (std::abs(pilot) * kPilotEnvInject);
    m_mpxMagnitude =
        (m_mpxMagnitude * kPilotEnvSmooth) + (std::abs(mpx) * kPilotEnvInject);
    const float phaseNow = m_liquidPilotPll.phase();
    const std::complex<float> vco(std::cos(phaseNow), std::sin(phaseNow));
    const std::complex<float> mixedPilot = pilot * std::conj(vco);
    const float error = std::atan2(mixedPilot.imag(), mixedPilot.real());
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

    m_pilotI =
        (m_pilotI * kPilotIqSmooth) + (mixedPilot.real() * kPilotIqInject);
    m_pilotQ =
        (m_pilotQ * kPilotIqSmooth) + (mixedPilot.imag() * kPilotIqInject);
    const float pilotMagNow =
        std::sqrt((m_pilotI * m_pilotI) + (m_pilotQ * m_pilotQ));
    const float pilotRatioNow =
        m_pilotBandMagnitude / std::max(m_mpxMagnitude, 1e-3f);
    const float pilotCoherenceNow =
        pilotMagNow / std::max(m_pilotBandMagnitude, 1e-4f);
    const float pilotResidual = mixedPilot.imag();
    m_pilotResidualMagnitude =
        (m_pilotResidualMagnitude * kPilotEnvSmooth) +
        (std::abs(pilotResidual) * kPilotEnvInject);
    const float pilotResidualRatioNow =
        m_pilotResidualMagnitude / std::max(pilotMagNow, 1e-4f);
    const float pllErrHzNow = std::abs(m_pllFreq - nominalPllFreq) *
                              static_cast<float>(m_inputRate) / (2.0f * kPi);

    const std::complex<float> delayedMpx = m_delayLine[m_delayPos];
    m_delayLine[m_delayPos] = std::complex<float>(mpx, 0.0f);
    m_delayPos++;
    if (m_delayPos >= m_delayLine.size()) {
      m_delayPos = 0;
    }

    // SDR++-style L-R recovery:
    // 1) take delayed MPX as complex (real, imag=0),
    // 2) multiply by conjugated PLL output twice (38 kHz downconversion),
    // 3) take real part and apply 2x gain.
    const std::complex<float> pll(std::cos(m_pllPhase), std::sin(m_pllPhase));
    const std::complex<float> lrMixed =
        delayedMpx * std::conj(pll) * std::conj(pll);
    float monoRaw = delayedMpx.real();
    const float lrRaw = 2.0f * lrMixed.real();

    // 19 kHz pilot canceller. Two-tap LMS: subtracts gI·cos(pllPhase) +
    // gQ·sin(pllPhase) from the mono path. Using both in-phase and quadrature
    // references lets the canceller kill the pilot regardless of the
    // single-sample PLL timing offset between phaseNow (used inside the loop
    // filter) and phaseNext (used here for the L-R recovery). Convergence time
    // constant is ~2/μ samples ≈ 80 ms at 256 kHz; steady-state residual is
    // gradient-noise-limited by the cross-correlation of mono program content
    // with the pilot reference (smaller μ = less noise, slower lock).
    if (m_pilotCancellerEnabled) {
      const float refI = pll.real();
      const float refQ = pll.imag();
      const float monoAfter =
          monoRaw - (m_pilotCancelGainI * refI + m_pilotCancelGainQ * refQ);
      if (m_stereoDetected || m_forceStereo) {
        constexpr float kPilotCancelMu = 1.0e-4f;
        m_pilotCancelGainI += kPilotCancelMu * monoAfter * refI;
        m_pilotCancelGainQ += kPilotCancelMu * monoAfter * refQ;
      }
      monoRaw = monoAfter;
    }
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

    // Hi-Blend: scale L-R by blend (so blend=0 mutes stereo exactly) and
    // pass through a 2nd-order Butterworth LPF whose cutoff scales with
    // blend². Coefficients are redesigned per sample because m_stereoBlend
    // ramps across the block; the cost (~1 tan + a few muls) is dwarfed by
    // upstream filter cost. 12 dB/oct above cutoff cleanly rejects stereo
    // noise during weak reception.
    const float blendClamped = std::clamp(m_stereoBlend, 0.0f, 1.0f);
    const float cutoffHz = 200.0f + 14800.0f * blendClamped * blendClamped;
    const float omega = std::tan(kPiOverFs * cutoffHz);
    const float omega2 = omega * omega;
    const float invNorm = 1.0f / (1.0f + omega / kButterQ + omega2);
    m_hiBlendB0 = omega2 * invNorm;
    m_hiBlendB1 = 2.0f * m_hiBlendB0;
    m_hiBlendB2 = m_hiBlendB0;
    m_hiBlendA1 = 2.0f * (omega2 - 1.0f) * invNorm;
    m_hiBlendA2 = (1.0f - omega / kButterQ + omega2) * invNorm;

    const float lrScaled = lrRaw * blendClamped;
    const float biquadOut = m_hiBlendB0 * lrScaled + m_hiBlendZ1;
    m_hiBlendZ1 =
        m_hiBlendB1 * lrScaled - m_hiBlendA1 * biquadOut + m_hiBlendZ2;
    m_hiBlendZ2 = m_hiBlendB2 * lrScaled - m_hiBlendA2 * biquadOut;
    const float lrAdapted = biquadOut;

    const float leftRaw = 0.5f * (monoRaw + lrAdapted);
    const float rightRaw = 0.5f * (monoRaw - lrAdapted);
    m_lrAudioMagnitude =
        (m_lrAudioMagnitude * kPilotEnvSmooth) +
        (0.5f * std::abs(leftRaw - rightRaw) * kPilotEnvInject);

    left[outCount] = leftRaw;
    right[outCount] = rightRaw;
    outCount++;
  }

  const float pilotMag =
      std::sqrt((m_pilotI * m_pilotI) + (m_pilotQ * m_pilotQ));
  m_pilotMagnitude = pilotMag;

  const float mpxThreshold = m_stereoDetected ? kMpxMinHold : kMpxMinAcquire;
  const float pilotRatio =
      m_pilotBandMagnitude / std::max(m_mpxMagnitude, 1e-3f);
  const float pilotCoherence =
      m_pilotMagnitude / std::max(m_pilotBandMagnitude, 1e-4f);
  const float pilotResidualRatio =
      m_pilotResidualMagnitude / std::max(m_pilotMagnitude, 1e-4f);
  const float ratioThreshold =
      m_stereoDetected ? kPilotRatioHold : kPilotRatioAcquire;
  const float coherenceThreshold =
      m_stereoDetected ? kPilotCoherenceHold : kPilotCoherenceAcquire;
  const float residualThreshold =
      m_stereoDetected ? kPilotResidualHold : kPilotResidualAcquire;
  const bool pilotPresent =
      (m_mpxMagnitude > mpxThreshold) &&
      (pilotRatio > ratioThreshold) &&
      (pilotCoherence > coherenceThreshold) &&
      (pilotResidualRatio < residualThreshold);
  if (!m_forceStereo) {
    const float blockSec = static_cast<float>(numSamples) /
                           std::max(1.0f, static_cast<float>(m_inputRate));
    const float alphaAcquire =
        1.0f - std::exp(-blockSec / kPilotAcquireTauSec);
    const float alphaRelease =
        1.0f - std::exp(-blockSec / kPilotReleaseTauSec);
    const float target = pilotPresent ? 1.0f : 0.0f;
    const float alpha =
        (target > m_pilotConfidence) ? alphaAcquire : alphaRelease;
    m_pilotConfidence =
        std::clamp(m_pilotConfidence + (target - m_pilotConfidence) * alpha,
                   0.0f, 1.0f);

    if (!m_stereoDetected &&
        m_pilotConfidence >= kPilotAcquireConfidence) {
      m_stereoDetected = true;
      m_liquidPilotPll.setPLLBandwidth(kPilotPllBwHold);
    } else if (m_stereoDetected &&
               m_pilotConfidence <= kPilotHoldConfidence) {
      m_stereoDetected = false;
      m_liquidPilotPll.setPLLBandwidth(kPilotPllBwAcquire);
    }
  }

  const float calibrated = m_pilotMagnitude * 8.0f;
  m_pilotLevelTenthsKHz =
      std::clamp(static_cast<int>(std::round(calibrated * 750.0f)), 0, 750);
  return outCount;
}
