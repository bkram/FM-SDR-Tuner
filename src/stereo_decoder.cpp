#include "stereo_decoder.h"
#include <algorithm>

namespace {
constexpr float kPi = 3.14159265358979323846f;
constexpr int kPilotAcquireBlocks = 10;
constexpr int kPilotLossBlocks = 18;
constexpr float kMatrixScale = 0.5f;
constexpr float kPilotAbsAcquire = 0.0028f;
constexpr float kPilotAbsHold = 0.0018f;
constexpr float kPilotRatioAcquire = 0.060f;
constexpr float kPilotRatioHold = 0.035f;
constexpr float kMpxMinAcquire = 0.008f;
constexpr float kMpxMinHold = 0.004f;
constexpr float kPilotCoherenceAcquire = 0.25f;
constexpr float kPilotCoherenceHold = 0.16f;
constexpr float kPllLockAcquireHz = 120.0f;
constexpr float kPllLockHoldHz = 220.0f;
}  // namespace

StereoDecoder::StereoDecoder(int inputRate, int outputRate)
    : m_inputRate(inputRate)
    , m_outputRate(outputRate)
    , m_downsampleFactor(std::max(1, inputRate / outputRate))
    , m_stereoDetected(false)
    , m_forceStereo(false)
    , m_forceMono(false)
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
    , m_pllAlpha(0.01f)
    , m_pllBeta(0.0001f)
    , m_pilotCount(0)
    , m_pilotLossCount(0)
    , m_pilotHistPos(0)
    , m_leftHistPos(0)
    , m_rightHistPos(0)
    , m_delayPos(0)
    , m_delaySamples(0)
    , m_decimPhase(0)
    , m_deemphAlpha(1.0f)
    , m_deemphStateLeft(0.0f)
    , m_deemphStateRight(0.0f) {
    m_pilotTaps = designBandPass(18750.0, 19250.0, 3000.0);
    m_pilotHistory.assign(m_pilotTaps.size(), 0.0f);
    m_audioTaps = designLowPass(15000.0, 4000.0);
    m_leftHistory.assign(m_audioTaps.size(), 0.0f);
    m_rightHistory.assign(m_audioTaps.size(), 0.0f);

    // Match SDR++ broadcast_fm delay around pilot filter latency.
    m_delaySamples = static_cast<int>((m_pilotTaps.size() > 0) ? ((m_pilotTaps.size() - 1) / 2 + 1) : 0);
    m_delayLine.assign(static_cast<size_t>(std::max(1, m_delaySamples + 1)), 0.0f);

    setDeemphasis(75);
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
    m_leftHistPos = 0;
    m_rightHistPos = 0;
    m_delayPos = 0;
    m_decimPhase = 0;
    m_deemphStateLeft = 0.0f;
    m_deemphStateRight = 0.0f;
    std::fill(m_pilotHistory.begin(), m_pilotHistory.end(), 0.0f);
    std::fill(m_leftHistory.begin(), m_leftHistory.end(), 0.0f);
    std::fill(m_rightHistory.begin(), m_rightHistory.end(), 0.0f);
    std::fill(m_delayLine.begin(), m_delayLine.end(), 0.0f);
}

void StereoDecoder::setForceStereo(bool force) {
    m_forceStereo = force;
}

void StereoDecoder::setForceMono(bool force) {
    m_forceMono = force;
}

void StereoDecoder::setDeemphasis(int tau_us) {
    if (tau_us <= 0) {
        m_deemphAlpha = 1.0f;
        return;
    }
    const float tau = static_cast<float>(tau_us) * 1e-6f;
    const float dt = 1.0f / static_cast<float>(m_outputRate);
    m_deemphAlpha = dt / (tau + dt);
}

size_t StereoDecoder::processAudio(const float* mono, float* left, float* right, size_t numSamples) {
    if (!mono || !left || !right || numSamples == 0) {
        return 0;
    }

    const float blendAttack = 1.0f - std::exp(-1.0f / (0.120f * static_cast<float>(m_inputRate)));
    const float blendRelease = 1.0f - std::exp(-1.0f / (0.030f * static_cast<float>(m_inputRate)));
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

        if (m_stereoDetected) {
            return std::clamp(0.15f + (0.85f * quality), 0.15f, 1.0f);
        }

        const bool prelockPilot = (m_mpxMagnitude > kMpxMinAcquire) &&
                                  (pilotMag > (kPilotAbsHold * 0.85f)) &&
                                  (pilotRatio > kPilotRatioHold) &&
                                  (pilotCoherence > kPilotCoherenceHold) &&
                                  (pllErrHz < kPllLockHoldHz);
        if (!prelockPilot) {
            return 0.0f;
        }
        return std::clamp(0.05f + (0.45f * quality), 0.05f, 0.50f);
    };

    size_t outCount = 0;
    for (size_t i = 0; i < numSamples; i++) {
        const float mpx = mono[i];

        // Pilot extraction and PLL tracking (SDR++ style: pilot BPF then PLL).
        const float pilot = filterSample(mpx, m_pilotTaps, m_pilotHistory, m_pilotHistPos);
        m_pilotBandMagnitude = (m_pilotBandMagnitude * 0.995f) + (std::abs(pilot) * 0.005f);
        m_mpxMagnitude = (m_mpxMagnitude * 0.995f) + (std::abs(mpx) * 0.005f);
        const float vcoI = std::cos(m_pllPhase);
        const float vcoQ = std::sin(m_pllPhase);
        const float error = pilot * vcoQ;

        m_pllFreq = std::clamp(m_pllFreq + (m_pllBeta * error), m_pllMinFreq, m_pllMaxFreq);
        m_pllPhase += m_pllFreq + (m_pllAlpha * error);
        if (m_pllPhase > 2.0f * kPi) {
            m_pllPhase -= 2.0f * kPi;
        } else if (m_pllPhase < 0.0f) {
            m_pllPhase += 2.0f * kPi;
        }

        m_pilotI = (m_pilotI * 0.995f) + ((pilot * vcoI) * 0.005f);
        m_pilotQ = (m_pilotQ * 0.995f) + ((pilot * vcoQ) * 0.005f);
        const float pilotMagNow = std::sqrt((m_pilotI * m_pilotI) + (m_pilotQ * m_pilotQ));
        const float pilotRatioNow = m_pilotBandMagnitude / std::max(m_mpxMagnitude, 1e-3f);
        const float pilotCoherenceNow = pilotMagNow / std::max(m_pilotBandMagnitude, 1e-4f);
        const float pllErrHzNow = std::abs(m_pllFreq - nominalPllFreq) * static_cast<float>(m_inputRate) / (2.0f * kPi);
        const float targetStereoBlend = computeBlendTarget(pilotMagNow, pilotRatioNow, pilotCoherenceNow, pllErrHzNow);

        const float delayedMpx = m_delayLine[m_delayPos];
        m_delayLine[m_delayPos] = mpx;
        m_delayPos = (m_delayPos + 1) % m_delayLine.size();

        // MPX mono carries (L+R), so normalize by 2 to match stereo loudness.
        const float monoNorm = delayedMpx * kMatrixScale;
        const float subcarrier = std::cos(2.0f * m_pllPhase);
        const float lr = 2.0f * delayedMpx * subcarrier;
        const float stereoLeft = (delayedMpx + lr) * kMatrixScale;
        const float stereoRight = (delayedMpx - lr) * kMatrixScale;

        const float blendAlpha = (targetStereoBlend > m_stereoBlend) ? blendAttack : blendRelease;
        m_stereoBlend += (targetStereoBlend - m_stereoBlend) * blendAlpha;

        float leftRaw = monoNorm + ((stereoLeft - monoNorm) * m_stereoBlend);
        float rightRaw = monoNorm + ((stereoRight - monoNorm) * m_stereoBlend);

        const float leftFilt = filterSample(leftRaw, m_audioTaps, m_leftHistory, m_leftHistPos);
        const float rightFilt = filterSample(rightRaw, m_audioTaps, m_rightHistory, m_rightHistPos);

        m_decimPhase++;
        if (m_decimPhase < m_downsampleFactor) {
            continue;
        }
        m_decimPhase = 0;

        // SDR++ applies deemphasis after AF resampling.
        m_deemphStateLeft = (m_deemphAlpha * leftFilt) + ((1.0f - m_deemphAlpha) * m_deemphStateLeft);
        m_deemphStateRight = (m_deemphAlpha * rightFilt) + ((1.0f - m_deemphAlpha) * m_deemphStateRight);
        left[outCount] = std::clamp(m_deemphStateLeft, -1.0f, 1.0f);
        right[outCount] = std::clamp(m_deemphStateRight, -1.0f, 1.0f);
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
    if (taps.empty() || history.empty()) {
        return input;
    }

    history[pos] = input;
    pos = (pos + 1) % history.size();

    float out = 0.0f;
    size_t idx = pos;
    for (size_t i = 0; i < taps.size(); i++) {
        idx = (idx == 0) ? (history.size() - 1) : (idx - 1);
        out += taps[i] * history[idx];
    }
    return out;
}

std::vector<float> StereoDecoder::designLowPass(double cutoffHz, double transitionHz) const {
    int tapCount = static_cast<int>(std::ceil(3.8 * static_cast<double>(m_inputRate) / transitionHz));
    tapCount = std::clamp(tapCount, 63, 511);
    if ((tapCount % 2) == 0) {
        tapCount++;
    }

    std::vector<float> taps(static_cast<size_t>(tapCount), 0.0f);
    const int mid = tapCount / 2;
    const double omega = 2.0 * M_PI * cutoffHz / static_cast<double>(m_inputRate);
    double sum = 0.0;

    for (int n = 0; n < tapCount; n++) {
        const int m = n - mid;
        const double sinc = (m == 0)
            ? (omega / M_PI)
            : (std::sin(omega * static_cast<double>(m)) / (M_PI * static_cast<double>(m)));
        const double h = sinc * windowNuttall(n, tapCount);
        taps[static_cast<size_t>(n)] = static_cast<float>(h);
        sum += h;
    }

    if (std::abs(sum) > 1e-12) {
        const float invSum = static_cast<float>(1.0 / sum);
        for (float& tap : taps) {
            tap *= invSum;
        }
    }

    return taps;
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
            h = (std::sin(2.0 * M_PI * highHz * mm / fs) - std::sin(2.0 * M_PI * lowHz * mm / fs)) / (M_PI * mm);
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
    const double x = 2.0 * M_PI * static_cast<double>(n) / static_cast<double>(count - 1);
    return static_cast<float>(0.355768
                            - 0.487396 * std::cos(x)
                            + 0.144232 * std::cos(2.0 * x)
                            - 0.012604 * std::cos(3.0 * x));
}
