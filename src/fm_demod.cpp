#include "fm_demod.h"
#include <array>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <limits>

#if defined(__AVX2__) && defined(__FMA__)
#include <immintrin.h>
#endif
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#endif

namespace {
const std::array<float, 256>& iqNormLut() {
    static const std::array<float, 256> lut = []() {
        std::array<float, 256> out{};
        for (int v = 0; v < 256; v++) {
            out[static_cast<size_t>(v)] = (static_cast<float>(v) - 127.5f) / 127.5f;
        }
        return out;
    }();
    return lut;
}

const std::array<float, 256 * 256>& iqInvPowerLut() {
    static const std::array<float, 256 * 256> lut = []() {
        constexpr float kEps = 1e-12f;
        std::array<float, 256 * 256> out{};
        for (int i = 0; i < 256; i++) {
            const float iNorm = (static_cast<float>(i) - 127.5f) / 127.5f;
            for (int q = 0; q < 256; q++) {
                const float qNorm = (static_cast<float>(q) - 127.5f) / 127.5f;
                const float denom = (iNorm * iNorm) + (qNorm * qNorm) + kEps;
                out[static_cast<size_t>((i << 8) | q)] = 1.0f / denom;
            }
        }
        return out;
    }();
    return lut;
}

float firLinearScalar(const float* tapsRev,
                      const float* historyLinear,
                      size_t tapCount) {
    float sample = 0.0f;
    for (size_t t = 0; t < tapCount; t++) {
        sample += tapsRev[t] * historyLinear[t];
    }
    return sample;
}

#if defined(__AVX2__) && defined(__FMA__)
float firLinearAvx2Fma(const float* tapsRev,
                       const float* historyLinear,
                       size_t tapCount) {
    __m256 acc = _mm256_setzero_ps();
    size_t t = 0;

    for (; t + 8 <= tapCount; t += 8) {
        const __m256 tapVec = _mm256_loadu_ps(tapsRev + t);
        const __m256 histVec = _mm256_loadu_ps(historyLinear + t);
        acc = _mm256_fmadd_ps(tapVec, histVec, acc);
    }

    alignas(32) float sumArr[8];
    _mm256_store_ps(sumArr, acc);
    float sum = sumArr[0] + sumArr[1] + sumArr[2] + sumArr[3]
              + sumArr[4] + sumArr[5] + sumArr[6] + sumArr[7];

    for (; t < tapCount; t++) {
        sum += tapsRev[t] * historyLinear[t];
    }
    return sum;
}
#endif

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
float firLinearNeon(const float* tapsRev,
                    const float* historyLinear,
                    size_t tapCount) {
    float32x4_t acc = vdupq_n_f32(0.0f);
    size_t t = 0;

    for (; t + 4 <= tapCount; t += 4) {
        const float32x4_t tapVec = vld1q_f32(tapsRev + t);
        const float32x4_t histVec = vld1q_f32(historyLinear + t);
#if defined(__aarch64__)
        acc = vfmaq_f32(acc, tapVec, histVec);
#else
        acc = vmlaq_f32(acc, tapVec, histVec);
#endif
    }

    float sum = 0.0f;
#if defined(__aarch64__)
    sum = vaddvq_f32(acc);
#else
    float tmp[4];
    vst1q_f32(tmp, acc);
    sum = tmp[0] + tmp[1] + tmp[2] + tmp[3];
#endif
    for (; t < tapCount; t++) {
        sum += tapsRev[t] * historyLinear[t];
    }
    return sum;
}
#endif
}  // namespace

FMDemod::FMDemod(int inputRate, int outputRate)
    : m_inputRate(inputRate)
    , m_outputRate(outputRate)
    , m_downsampleFactor(std::max(1, inputRate / outputRate))
    , m_demodMode(DemodMode::Fast)
    , m_lastPhase(0.0f)
    , m_haveLastPhase(false)
    , m_prevI(0.0f)
    , m_prevQ(0.0f)
    , m_havePrevIQ(false)
    , m_deviation(75000.0)
    , m_invDeviation(0.0)
    , m_deemphAlpha(1.0f)
    , m_deemphasisState(0.0f)
    , m_bandwidthMode(0)
    , m_audioHistPos(0)
    , m_decimPhase(0) {
    initAudioFilter();
    setDeviation(75000.0);
    setDeemphasis(75);
}

FMDemod::~FMDemod() = default;

void FMDemod::setDeemphasis(int tau_us) {
    if (tau_us <= 0) {
        m_deemphAlpha = 1.0f;
        return;
    }
    const float tau = static_cast<float>(tau_us) * 1e-6f;
    const float dt = 1.0f / static_cast<float>(m_outputRate);
    m_deemphAlpha = dt / (tau + dt);
}

void FMDemod::setDeviation(double deviation) {
    m_deviation = deviation;
    m_invDeviation = m_inputRate / (2.0 * M_PI * deviation);
}

void FMDemod::reset() {
    m_lastPhase = 0.0f;
    m_haveLastPhase = false;
    m_prevI = 0.0f;
    m_prevQ = 0.0f;
    m_havePrevIQ = false;
    m_deemphasisState = 0.0f;
    m_audioHistPos = 0;
    m_decimPhase = 0;
    std::fill(m_audioHistoryLinear.begin(), m_audioHistoryLinear.end(), 0.0f);
}

void FMDemod::initAudioFilter() {
    // Default wide channel filter for MPX path.
    rebuildAudioFilter(120000.0);
}

void FMDemod::rebuildAudioFilter(double cutoffHz) {
    // Match SDR++ intent: post-demod low-pass around 15 kHz before AF resampling.
    constexpr double transitionHz = 4000.0;
    int tapCount = static_cast<int>(std::ceil(3.8 * static_cast<double>(m_inputRate) / transitionHz));
    tapCount = std::clamp(tapCount, 63, 1023);
    if ((tapCount % 2) == 0) {
        tapCount++;
    }

    m_audioTaps.assign(static_cast<size_t>(tapCount), 0.0f);
    const int mid = tapCount / 2;
    const double omega = 2.0 * M_PI * cutoffHz / static_cast<double>(m_inputRate);
    double sum = 0.0;
    for (int n = 0; n < tapCount; n++) {
        const int m = n - mid;
        double sinc = 0.0;
        if (m == 0) {
            sinc = omega / M_PI;
        } else {
            sinc = std::sin(omega * static_cast<double>(m)) / (M_PI * static_cast<double>(m));
        }

        // Nuttall window (same family used by SDR++ tap generation).
        const double x = 2.0 * M_PI * static_cast<double>(n) / static_cast<double>(tapCount - 1);
        const double window = 0.355768
                            - 0.487396 * std::cos(x)
                            + 0.144232 * std::cos(2.0 * x)
                            - 0.012604 * std::cos(3.0 * x);

        const double h = sinc * window;
        m_audioTaps[static_cast<size_t>(n)] = static_cast<float>(h);
        sum += h;
    }

    if (std::abs(sum) > 1e-12) {
        const float invSum = static_cast<float>(1.0 / sum);
        for (float& tap : m_audioTaps) {
            tap *= invSum;
        }
    }

    m_audioTapsRev.resize(m_audioTaps.size());
    std::reverse_copy(m_audioTaps.begin(), m_audioTaps.end(), m_audioTapsRev.begin());
    m_audioHistoryLinear.assign(m_audioTaps.size() * 2, 0.0f);
    m_audioHistPos = 0;
    m_decimPhase = 0;
}

void FMDemod::setBandwidthMode(int mode) {
    static constexpr int kTefBwHz[] = {
        311000, 287000, 254000, 236000, 217000, 200000, 184000, 168000,
        151000, 133000, 114000, 97000, 84000, 72000, 64000, 56000, 0
    };
    const int clipped = std::clamp(mode, 0, static_cast<int>(std::size(kTefBwHz) - 1));
    setBandwidthHz(kTefBwHz[clipped]);
}

void FMDemod::setBandwidthHz(int bwHz) {
    // TEF FM W table (Hz) mapped to MPX/channel low-pass cutoffs (Hz)
    // for the no-downsample (stereo/RDS) path.
    static constexpr int kTefBwHz[] = {
        311000, 287000, 254000, 236000, 217000, 200000, 184000, 168000,
        151000, 133000, 114000, 97000, 84000, 72000, 64000, 56000, 0
    };

    int selected = static_cast<int>(std::size(kTefBwHz) - 1);
    if (bwHz > 0) {
        int minDiff = std::numeric_limits<int>::max();
        for (int i = 0; i < static_cast<int>(std::size(kTefBwHz)) - 1; i++) {
            const int diff = std::abs(kTefBwHz[i] - bwHz);
            if (diff < minDiff) {
                minDiff = diff;
                selected = i;
            }
        }
    }

    if (selected == m_bandwidthMode) {
        return;
    }
    m_bandwidthMode = selected;
    const int selectedBwHz = kTefBwHz[selected];
    // Approximate channel selectivity from TEF bandwidth to MPX cutoff:
    // BW/2 in baseband, clamped to preserve pilot/stereo operation.
    const double cutoffHz = (selectedBwHz > 0)
                                ? std::clamp(static_cast<double>(selectedBwHz) * 0.5, 30000.0, 120000.0)
                                : 120000.0;
    rebuildAudioFilter(cutoffHz);
}

void FMDemod::setDemodMode(DemodMode mode) {
    if (m_demodMode == mode) {
        return;
    }
    m_demodMode = mode;
    m_lastPhase = 0.0f;
    m_haveLastPhase = false;
    m_prevI = 0.0f;
    m_prevQ = 0.0f;
    m_havePrevIQ = false;
}

void FMDemod::demodulate(const uint8_t* iq, float* audio, size_t len) {
    constexpr float kPi = 3.14159265358979323846f;
    constexpr float kTwoPi = 6.28318530717958647692f;
    const auto& kIqNorm = iqNormLut();
    const auto& kInvPower = iqInvPowerLut();
    const uint8_t* iqPtr = iq;

    if (m_demodMode == DemodMode::Fast) {
        float prevI = m_prevI;
        float prevQ = m_prevQ;
        bool havePrev = m_havePrevIQ;
        for (size_t i = 0; i < len; i++) {
            // rtl_tcp provides unsigned 8-bit IQ centered at 127.5.
            const uint8_t iRaw = iqPtr[0];
            const uint8_t qRaw = iqPtr[1];
            const float i_val = kIqNorm[iRaw];
            const float q_val = kIqNorm[qRaw];
            iqPtr += 2;

            if (!havePrev) {
                audio[i] = 0.0f;
                prevI = i_val;
                prevQ = q_val;
                havePrev = true;
                continue;
            }

            // Fast quadrature FM discriminator (avoids atan2 in the hot loop).
            const float dI = i_val - prevI;
            const float dQ = q_val - prevQ;
            const float invPower = kInvPower[static_cast<size_t>((static_cast<uint16_t>(iRaw) << 8) | qRaw)];
            const float delta = ((i_val * dQ) - (q_val * dI)) * invPower;
            audio[i] = delta * m_invDeviation;
            prevI = i_val;
            prevQ = q_val;
        }
        m_prevI = prevI;
        m_prevQ = prevQ;
        m_havePrevIQ = havePrev;
        return;
    }

    float lastPhase = m_lastPhase;
    bool havePhase = m_haveLastPhase;
    for (size_t i = 0; i < len; i++) {
        const float i_val = kIqNorm[iqPtr[0]];
        const float q_val = kIqNorm[iqPtr[1]];
        iqPtr += 2;

        const float phase = std::atan2f(q_val, i_val);
        if (!havePhase) {
            audio[i] = 0.0f;
            lastPhase = phase;
            havePhase = true;
            continue;
        }

        float delta = phase - lastPhase;
        if (delta > kPi) {
            delta -= kTwoPi;
        } else if (delta <= -kPi) {
            delta += kTwoPi;
        }
        audio[i] = delta * m_invDeviation;
        lastPhase = phase;
    }
    m_lastPhase = lastPhase;
    m_haveLastPhase = havePhase;
}

size_t FMDemod::downsampleAudio(const float* demod, float* audio, size_t numSamples) {
    if (m_audioTaps.empty() || m_audioTapsRev.empty() || m_audioHistoryLinear.empty()) {
        return 0;
    }

    const size_t tapCount = m_audioTaps.size();
    size_t outCount = 0;

    for (size_t i = 0; i < numSamples; i++) {
        m_audioHistoryLinear[m_audioHistPos] = demod[i];
        m_audioHistoryLinear[m_audioHistPos + tapCount] = demod[i];
        m_audioHistPos++;
        if (m_audioHistPos >= tapCount) {
            m_audioHistPos = 0;
        }

        m_decimPhase++;
        if (m_decimPhase < m_downsampleFactor) {
            continue;
        }
        m_decimPhase = 0;

        const float* historyWindow = &m_audioHistoryLinear[m_audioHistPos];
#if defined(__AVX2__) && defined(__FMA__)
        const float sample = firLinearAvx2Fma(m_audioTapsRev.data(), historyWindow, tapCount);
#elif defined(__ARM_NEON) || defined(__ARM_NEON__)
        const float sample = firLinearNeon(m_audioTapsRev.data(), historyWindow, tapCount);
#else
        const float sample = firLinearScalar(m_audioTapsRev.data(), historyWindow, tapCount);
#endif

        // SDR++ applies deemphasis in post-processing after AF resampling.
        m_deemphasisState = (m_deemphAlpha * sample) + ((1.0f - m_deemphAlpha) * m_deemphasisState);
        audio[outCount++] = m_deemphasisState;
    }

    return outCount;
}

void FMDemod::process(const uint8_t* iq, float* audio, size_t numSamples) {
    if (m_demodScratch.size() < numSamples) {
        m_demodScratch.resize(numSamples);
    }
    demodulate(iq, m_demodScratch.data(), numSamples);
    downsampleAudio(m_demodScratch.data(), audio, numSamples);
}

void FMDemod::processNoDownsample(const uint8_t* iq, float* audio, size_t numSamples) {
    if (m_audioTaps.empty() || m_audioTapsRev.empty() || m_audioHistoryLinear.empty()) {
        demodulate(iq, audio, numSamples);
        return;
    }

    if (m_demodScratch.size() < numSamples) {
        m_demodScratch.resize(numSamples);
    }
    demodulate(iq, m_demodScratch.data(), numSamples);

    const size_t tapCount = m_audioTaps.size();
    for (size_t i = 0; i < numSamples; i++) {
        m_audioHistoryLinear[m_audioHistPos] = m_demodScratch[i];
        m_audioHistoryLinear[m_audioHistPos + tapCount] = m_demodScratch[i];
        m_audioHistPos++;
        if (m_audioHistPos >= tapCount) {
            m_audioHistPos = 0;
        }

        const float* historyWindow = &m_audioHistoryLinear[m_audioHistPos];
#if defined(__AVX2__) && defined(__FMA__)
        const float sample = firLinearAvx2Fma(m_audioTapsRev.data(), historyWindow, tapCount);
#elif defined(__ARM_NEON) || defined(__ARM_NEON__)
        const float sample = firLinearNeon(m_audioTapsRev.data(), historyWindow, tapCount);
#else
        const float sample = firLinearScalar(m_audioTapsRev.data(), historyWindow, tapCount);
#endif
        audio[i] = sample;
    }
}
