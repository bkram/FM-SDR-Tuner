#include "stereo_decoder.h"
#include <algorithm>

namespace {
constexpr float kPi = 3.14159265358979323846f;
constexpr int kPilotAcquireBlocks = 6;
constexpr int kPilotLossBlocks = 24;
constexpr float kMatrixScale = 0.5f;
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
constexpr float kPilotEnvSmooth = 0.9995f;
constexpr float kPilotEnvInject = 1.0f - kPilotEnvSmooth;
constexpr float kPilotIqSmooth = 0.9995f;
constexpr float kPilotIqInject = 1.0f - kPilotIqSmooth;

float dotProduct(const float* a, const float* b, size_t n) {
    float sum = 0.0f;
    for (size_t i = 0; i < n; i++) {
        sum += a[i] * b[i];
    }
    return sum;
}
}  // namespace

StereoDecoder::StereoDecoder(int inputRate, int /*outputRate*/)
    : m_inputRate(inputRate)
    , m_stereoDetected(false)
    , m_forceStereo(false)
    , m_forceMono(false)
    , m_blendMode(BlendMode::Normal)
    , m_pilotMagnitude(0.0f)
    , m_pilotBandMagnitude(0.0f)
    , m_mpxMagnitude(0.0f)
    , m_stereoBlend(0.0f)
    , m_pilotLevelTenthsKHz(0)
    , m_pilotI(0.0f)
    , m_pilotQ(0.0f)
    , m_pllPhase(0.0f)
    , m_pllFreq(2.0f * kPi * 19000.0f / static_cast<float>(inputRate))
    , m_pllMinFreq(2.0f * kPi * 18750.0f / static_cast<float>(inputRate))
    , m_pllMaxFreq(2.0f * kPi * 19250.0f / static_cast<float>(inputRate))
    , m_pilotCount(0)
    , m_pilotLossCount(0)
    , m_pilotHistPos(0)
    , m_delayPos(0)
    , m_delaySamples(0)
{
    m_pilotTaps = designBandPass(18750.0, 19250.0, 3000.0);
    m_pilotTapsRev = m_pilotTaps;
    std::reverse(m_pilotTapsRev.begin(), m_pilotTapsRev.end());
    m_pilotHistory.assign(m_pilotTapsRev.size() * 2, 0.0f);
    const float audioCutoffNorm = std::clamp(15000.0f / static_cast<float>(m_inputRate), 0.01f, 0.45f);
    m_liquidLeftAudioFilter.init(121, audioCutoffNorm);
    m_liquidRightAudioFilter.init(121, audioCutoffNorm);

    m_delaySamples = static_cast<int>((m_pilotTaps.size() > 0) ? ((m_pilotTaps.size() - 1) / 2) : 0);
    m_delayLine.assign(static_cast<size_t>(std::max(1, m_delaySamples + 1)), 0.0f);
    const float nominalPllFreq = 2.0f * kPi * 19000.0f / static_cast<float>(m_inputRate);
    m_liquidPilotPll.init(LIQUID_VCO, nominalPllFreq);
    m_liquidPilotPll.setPLLBandwidth(0.01f);
}

StereoDecoder::~StereoDecoder() = default;

void StereoDecoder::reset() {
    m_stereoDetected = false;
    m_pilotMagnitude = 0.0f;
    m_pilotBandMagnitude = 0.0f;
    m_mpxMagnitude = 0.0f;
    m_stereoBlend = 0.0f;
    m_pilotLevelTenthsKHz = 0;
    m_pilotI = 0.0f;
    m_pilotQ = 0.0f;
    m_pllPhase = 0.0f;
    m_pllFreq = 2.0f * kPi * 19000.0f / static_cast<float>(m_inputRate);
    m_pilotCount = 0;
    m_pilotLossCount = 0;
    m_pilotHistPos = 0;
    m_delayPos = 0;
    std::fill(m_pilotHistory.begin(), m_pilotHistory.end(), 0.0f);
    std::fill(m_delayLine.begin(), m_delayLine.end(), 0.0f);
    m_liquidPilotPll.reset();
    m_liquidLeftAudioFilter.reset();
    m_liquidRightAudioFilter.reset();
}

void StereoDecoder::setForceStereo(bool force) {
    m_forceStereo = force;
}

void StereoDecoder::setForceMono(bool force) {
    m_forceMono = force;
}

size_t StereoDecoder::processAudio(const float* mono, float* left, float* right, size_t numSamples) {
    if (!mono || !left || !right || numSamples == 0) {
        return 0;
    }

    float attackTau = 0.120f;
    float releaseTau = 0.030f;
    float lowQualityGate = 0.85f;
    float lockFloor = 0.02f;
    float prelockMax = 0.30f;
    if (m_blendMode == BlendMode::Soft) {
        attackTau = 0.090f;
        releaseTau = 0.040f;
        lowQualityGate = 0.75f;
        lockFloor = 0.08f;
        prelockMax = 0.45f;
    } else if (m_blendMode == BlendMode::Aggressive) {
        attackTau = 0.180f;
        releaseTau = 0.015f;
        lowQualityGate = 0.95f;
        lockFloor = 0.00f;
        prelockMax = 0.18f;
    }

    const float blendAttack = 1.0f - std::exp(-1.0f / (attackTau * static_cast<float>(m_inputRate)));
    const float blendRelease = 1.0f - std::exp(-1.0f / (releaseTau * static_cast<float>(m_inputRate)));
    const float nominalPllFreq = 2.0f * kPi * 19000.0f / static_cast<float>(m_inputRate);
    auto computeBlendTarget = [&](float pilotMag, float pilotRatio, float pilotCoherence, float pllErrHz) -> float {
        if (m_forceMono) {
            return 0.0f;
        }
        if (m_forceStereo) {
            return 1.0f;
        }

        const float absQ = std::clamp((pilotMag - kPilotAbsHold) /
                                      std::max(kPilotAbsAcquire - kPilotAbsHold, 1e-4f), 0.0f, 1.0f);
        const float ratioQ = std::clamp((pilotRatio - kPilotRatioHold) /
                                        std::max(kPilotRatioAcquire - kPilotRatioHold, 1e-4f), 0.0f, 1.0f);
        const float cohQ = std::clamp((pilotCoherence - kPilotCoherenceHold) /
                                      std::max(kPilotCoherenceAcquire - kPilotCoherenceHold, 1e-4f), 0.0f, 1.0f);
        const float pllQ = std::clamp((kPllLockHoldHz - pllErrHz) /
                                      std::max(kPllLockHoldHz - kPllLockAcquireHz, 1e-3f), 0.0f, 1.0f);
        const float quality = std::min(absQ, std::min(ratioQ, std::min(cohQ, pllQ)));
        float qualityShaped = quality * quality;
        if (m_blendMode == BlendMode::Soft) {
            qualityShaped = std::sqrt(std::max(0.0f, quality));
        } else if (m_blendMode == BlendMode::Aggressive) {
            qualityShaped = quality * quality * quality;
        }

        // Drop to mono quickly when pilot quality degrades to avoid noisy/choppy stereo.
        if (pilotRatio < (kPilotRatioHold * lowQualityGate) ||
            pilotCoherence < (kPilotCoherenceHold * lowQualityGate) ||
            pllErrHz > (kPllLockHoldHz * 1.10f)) {
            return 0.0f;
        }

        if (m_stereoDetected) {
            return std::clamp(lockFloor + ((1.0f - lockFloor) * qualityShaped), 0.0f, 1.0f);
        }

        const bool prelockPilot = (m_mpxMagnitude > kMpxMinAcquire) &&
                                  (pilotMag > (kPilotAbsHold * 0.85f)) &&
                                  (pilotRatio > kPilotRatioHold) &&
                                  (pilotCoherence > kPilotCoherenceHold) &&
                                  (pllErrHz < kPllLockHoldHz);
        if (!prelockPilot) {
            return 0.0f;
        }
        return std::clamp(0.02f + ((prelockMax - 0.02f) * qualityShaped), 0.0f, prelockMax);
    };

    size_t outCount = 0;
    for (size_t i = 0; i < numSamples; i++) {
        const float mpx = mono[i];

        const float pilot = filterSample(mpx, m_pilotTapsRev, m_pilotHistory, m_pilotHistPos);
        m_pilotBandMagnitude = (m_pilotBandMagnitude * kPilotEnvSmooth) + (std::abs(pilot) * kPilotEnvInject);
        m_mpxMagnitude = (m_mpxMagnitude * kPilotEnvSmooth) + (std::abs(mpx) * kPilotEnvInject);
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
        const float pilotMagNow = std::sqrt((m_pilotI * m_pilotI) + (m_pilotQ * m_pilotQ));
        const float pilotRatioNow = m_pilotBandMagnitude / std::max(m_mpxMagnitude, 1e-3f);
        const float pilotCoherenceNow = pilotMagNow / std::max(m_pilotBandMagnitude, 1e-4f);
        const float pllErrHzNow = std::abs(m_pllFreq - nominalPllFreq) * static_cast<float>(m_inputRate) / (2.0f * kPi);
        const float targetStereoBlend = computeBlendTarget(pilotMagNow, pilotRatioNow, pilotCoherenceNow, pllErrHzNow);

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
        const float monoNorm = delayedMpx * kMatrixScale;
        const float pllRe = std::cos(m_pllPhase);
        const float pllIm = std::sin(m_pllPhase);
        const float cos2 = (pllRe * pllRe) - (pllIm * pllIm);
        const float lr = 2.0f * delayedMpx * cos2;
        const float stereoLeft = (delayedMpx + lr) * kMatrixScale;
        const float stereoRight = (delayedMpx - lr) * kMatrixScale;

        const float blendAlpha = (targetStereoBlend > m_stereoBlend) ? blendAttack : blendRelease;
        m_stereoBlend += (targetStereoBlend - m_stereoBlend) * blendAlpha;

        float leftRaw = monoNorm + ((stereoLeft - monoNorm) * m_stereoBlend);
        float rightRaw = monoNorm + ((stereoRight - monoNorm) * m_stereoBlend);

        m_liquidLeftAudioFilter.push(std::complex<float>(leftRaw, 0.0f));
        m_liquidRightAudioFilter.push(std::complex<float>(rightRaw, 0.0f));
        const float leftFilt = m_liquidLeftAudioFilter.execute().real();
        const float rightFilt = m_liquidRightAudioFilter.execute().real();

        left[outCount] = leftFilt;
        right[outCount] = rightFilt;
        outCount++;
    }

    const float pilotMag = std::sqrt((m_pilotI * m_pilotI) + (m_pilotQ * m_pilotQ));
    m_pilotMagnitude = (m_pilotMagnitude * 0.9f) + (pilotMag * 0.1f);

    const float mpxThreshold = m_stereoDetected ? kMpxMinHold : kMpxMinAcquire;
    const float pilotRatio = m_pilotBandMagnitude / std::max(m_mpxMagnitude, 1e-3f);
    const float pilotCoherence = m_pilotMagnitude / std::max(m_pilotBandMagnitude, 1e-4f);
    const float absThreshold = m_stereoDetected ? kPilotAbsHold : kPilotAbsAcquire;
    const float ratioThreshold = m_stereoDetected ? kPilotRatioHold : kPilotRatioAcquire;
    const float coherenceThreshold = m_stereoDetected ? kPilotCoherenceHold : kPilotCoherenceAcquire;
    const float pllErrHz = std::abs(m_pllFreq - nominalPllFreq) * static_cast<float>(m_inputRate) / (2.0f * kPi);
    const float pllThreshold = m_stereoDetected ? kPllLockHoldHz : kPllLockAcquireHz;
    const bool pilotPresent = (m_mpxMagnitude > mpxThreshold) &&
                              (m_pilotMagnitude > absThreshold) &&
                              (pilotRatio > ratioThreshold) &&
                              (pilotCoherence > coherenceThreshold) &&
                              (pllErrHz < pllThreshold);
    if (!m_forceStereo) {
        if (!m_stereoDetected) {
            if (pilotPresent) {
                m_pilotCount++;
                m_pilotLossCount = 0;
                if (m_pilotCount >= kPilotAcquireBlocks) {
                    m_stereoDetected = true;
                }
            } else {
                m_pilotCount = 0;
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
    m_pilotLevelTenthsKHz = std::clamp(static_cast<int>(std::round(calibrated * 750.0f)), 0, 750);
    return outCount;
}

float StereoDecoder::filterSample(float input, const std::vector<float>& taps, std::vector<float>& history, size_t& pos) {
    if (taps.empty() || history.size() < (taps.size() * 2)) {
        return input;
    }

    const size_t tapCount = taps.size();
    history[pos] = input;
    history[pos + tapCount] = input;
    pos++;
    if (pos >= tapCount) {
        pos = 0;
    }

    const float* x = &history[pos];
    const float* h = taps.data();
    return dotProduct(x, h, tapCount);
}

std::vector<float> StereoDecoder::designBandPass(double lowHz, double highHz, double transitionHz) const {
    int tapCount = static_cast<int>(std::ceil(3.8 * static_cast<double>(m_inputRate) / transitionHz));
    tapCount = std::clamp(tapCount, 63, 511);
    if ((tapCount % 2) == 0) {
        tapCount++;
    }

    std::vector<float> taps(static_cast<size_t>(tapCount), 0.0f);
    const int mid = tapCount / 2;
    const double fs = static_cast<double>(m_inputRate);
    double sumAbs = 0.0;

    for (int n = 0; n < tapCount; n++) {
        const int m = n - mid;
        double h = 0.0;
        if (m == 0) {
            h = 2.0 * (highHz - lowHz) / fs;
        } else {
            const double mm = static_cast<double>(m);
            h = (std::sin(2.0 * kPi * highHz * mm / fs) - std::sin(2.0 * kPi * lowHz * mm / fs)) / (kPi * mm);
        }
        h *= windowNuttall(n, tapCount);
        taps[static_cast<size_t>(n)] = static_cast<float>(h);
        sumAbs += std::abs(h);
    }

    if (sumAbs > 1e-12) {
        const float norm = static_cast<float>(1.0 / sumAbs);
        for (float& tap : taps) {
            tap *= norm;
        }
    }

    return taps;
}

float StereoDecoder::windowNuttall(int n, int count) const {
    const double x = 2.0 * kPi * static_cast<double>(n) / static_cast<double>(count - 1);
    return static_cast<float>(0.355768
                            - 0.487396 * std::cos(x)
                            + 0.144232 * std::cos(2.0 * x)
                            - 0.012604 * std::cos(3.0 * x));
}
