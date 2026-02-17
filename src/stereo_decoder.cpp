#include "stereo_decoder.h"
#include "cpu_features.h"
#include <algorithm>
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
#include <immintrin.h>
#endif
#if defined(__aarch64__) || defined(__arm__) || defined(_M_ARM64)
#include <arm_neon.h>
#endif

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

float dotProductScalar(const float* a, const float* b, size_t n) {
    float sum = 0.0f;
    for (size_t i = 0; i < n; i++) {
        sum += a[i] * b[i];
    }
    return sum;
}

#if defined(__aarch64__) || defined(__arm__) || defined(_M_ARM64)
float dotProductNeon(const float* a, const float* b, size_t n) {
    float32x4_t sumv = vdupq_n_f32(0.0f);
    size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        const float32x4_t av = vld1q_f32(a + i);
        const float32x4_t bv = vld1q_f32(b + i);
        sumv = vmlaq_f32(sumv, av, bv);
    }
#if defined(__aarch64__)
    float sum = vaddvq_f32(sumv);
#else
    float32x2_t sum2 = vadd_f32(vget_low_f32(sumv), vget_high_f32(sumv));
    sum2 = vpadd_f32(sum2, sum2);
    float sum = vget_lane_f32(sum2, 0);
#endif
    for (; i < n; i++) {
        sum += a[i] * b[i];
    }
    return sum;
}
#endif

#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
float dotProductSse(const float* a, const float* b, size_t n) {
    __m128 sumv = _mm_setzero_ps();
    size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        const __m128 av = _mm_loadu_ps(a + i);
        const __m128 bv = _mm_loadu_ps(b + i);
        sumv = _mm_add_ps(sumv, _mm_mul_ps(av, bv));
    }
    alignas(16) float lanes[4];
    _mm_store_ps(lanes, sumv);
    float sum = lanes[0] + lanes[1] + lanes[2] + lanes[3];
    for (; i < n; i++) {
        sum += a[i] * b[i];
    }
    return sum;
}

#if defined(__AVX2__) && defined(__FMA__)
float dotProductAvx2Fma(const float* a, const float* b, size_t n) {
    __m256 sumv = _mm256_setzero_ps();
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        const __m256 av = _mm256_loadu_ps(a + i);
        const __m256 bv = _mm256_loadu_ps(b + i);
        sumv = _mm256_fmadd_ps(av, bv, sumv);
    }
    alignas(32) float lanes[8];
    _mm256_store_ps(lanes, sumv);
    float sum = lanes[0] + lanes[1] + lanes[2] + lanes[3] + lanes[4] + lanes[5] + lanes[6] + lanes[7];
    for (; i < n; i++) {
        sum += a[i] * b[i];
    }
    return sum;
}
#endif
#endif
}  // namespace

StereoDecoder::StereoDecoder(int inputRate, int outputRate)
    : m_inputRate(inputRate)
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
    , m_useNeon(false)
    , m_useSse2(false)
    , m_useAvx2(false)
    , m_delayPos(0)
    , m_delaySamples(0) {
    m_pilotTaps = designBandPass(18750.0, 19250.0, 3000.0);
    m_pilotTapsRev = m_pilotTaps;
    std::reverse(m_pilotTapsRev.begin(), m_pilotTapsRev.end());
    m_pilotHistory.assign(m_pilotTapsRev.size() * 2, 0.0f);
    m_audioTaps = designLowPass(15000.0, 4000.0);
    m_audioTapsRev = m_audioTaps;
    std::reverse(m_audioTapsRev.begin(), m_audioTapsRev.end());
    m_leftHistory.assign(m_audioTapsRev.size() * 2, 0.0f);
    m_rightHistory.assign(m_audioTapsRev.size() * 2, 0.0f);
    const CPUFeatures cpu = detectCPUFeatures();
    m_useNeon = cpu.neon;
    m_useSse2 = cpu.sse2;
    m_useAvx2 = cpu.avx2 && cpu.fma;

    // Match SDR++ broadcast_fm delay around pilot filter latency.
    m_delaySamples = static_cast<int>((m_pilotTaps.size() > 0) ? ((m_pilotTaps.size() - 1) / 2 + 1) : 0);
    m_delayLine.assign(static_cast<size_t>(std::max(1, m_delaySamples + 1)), 0.0f);
    (void)outputRate;
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
        const float pilot = filterSample(mpx, m_pilotTapsRev, m_pilotHistory, m_pilotHistPos);
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

        const float leftFilt = filterSample(leftRaw, m_audioTapsRev, m_leftHistory, m_leftHistPos);
        const float rightFilt = filterSample(rightRaw, m_audioTapsRev, m_rightHistory, m_rightHistPos);

        left[outCount] = std::clamp(leftFilt, -1.0f, 1.0f);
        right[outCount] = std::clamp(rightFilt, -1.0f, 1.0f);
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

#if defined(__aarch64__) || defined(__arm__) || defined(_M_ARM64)
    if (m_useNeon) {
        return dotProductNeon(x, h, tapCount);
    }
#endif
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
#if defined(__AVX2__) && defined(__FMA__)
    if (m_useAvx2) {
        return dotProductAvx2Fma(x, h, tapCount);
    }
#endif
    if (m_useSse2) {
        return dotProductSse(x, h, tapCount);
    }
#endif
    return dotProductScalar(x, h, tapCount);
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
