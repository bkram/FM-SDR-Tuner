#include "fm_demod.h"
#include "cpu_features.h"
#include <array>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <limits>

#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
#include <immintrin.h>
#endif
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#endif

namespace {
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
#if defined(__has_attribute)
#if __has_attribute(target)
#define FMTUNER_FMDEMOD_HAS_AVX2_KERNEL 1
#define FMTUNER_FMDEMOD_AVX2_TARGET __attribute__((target("avx2,fma")))
#endif
#elif defined(__GNUC__)
#define FMTUNER_FMDEMOD_HAS_AVX2_KERNEL 1
#define FMTUNER_FMDEMOD_AVX2_TARGET __attribute__((target("avx2,fma")))
#endif
#endif
#ifndef FMTUNER_FMDEMOD_HAS_AVX2_KERNEL
#define FMTUNER_FMDEMOD_HAS_AVX2_KERNEL 0
#define FMTUNER_FMDEMOD_AVX2_TARGET
#endif

const std::array<float, 256>& iqNormLut() {
    static const std::array<float, 256> lut = []() {
        std::array<float, 256> out{};
        for (int v = 0; v < 256; v++) {
            out[static_cast<size_t>(v)] = (static_cast<float>(v) - 127.0f) / 127.5f;
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

#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
float firLinearSse2(const float* tapsRev,
                    const float* historyLinear,
                    size_t tapCount) {
    __m128 acc = _mm_setzero_ps();
    size_t t = 0;

    for (; t + 4 <= tapCount; t += 4) {
        const __m128 tapVec = _mm_loadu_ps(tapsRev + t);
        const __m128 histVec = _mm_loadu_ps(historyLinear + t);
        acc = _mm_add_ps(acc, _mm_mul_ps(tapVec, histVec));
    }

    alignas(16) float sumArr[4];
    _mm_store_ps(sumArr, acc);
    float sum = sumArr[0] + sumArr[1] + sumArr[2] + sumArr[3];
    for (; t < tapCount; t++) {
        sum += tapsRev[t] * historyLinear[t];
    }
    return sum;
}
#endif

#if FMTUNER_FMDEMOD_HAS_AVX2_KERNEL
FMTUNER_FMDEMOD_AVX2_TARGET float firLinearAvx2Fma(const float* tapsRev,
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
    , m_decimPhase(0)
    , m_decimHistPos(0)
    , m_iqHistPos(0)
    , m_iqPrevInI(0.0f)
    , m_iqPrevInQ(0.0f)
    , m_iqDcStateI(0.0f)
    , m_iqDcStateQ(0.0f)
    , m_clipping(false)
    , m_clippingRatio(0.0f)
    , m_useNeon(false)
    , m_useSse2(false)
    , m_useAvx2(false) {
    const CPUFeatures cpu = detectCPUFeatures();
    m_useNeon = cpu.neon;
    m_useSse2 = cpu.sse2;
    m_useAvx2 = cpu.avx2 && cpu.fma;
    initAudioFilter();
    initIQFilter();
    initDecimFilter();
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
    m_decimHistPos = 0;
    m_decimPhase = 0;
    std::fill(m_audioHistoryLinear.begin(), m_audioHistoryLinear.end(), 0.0f);
    std::fill(m_decimHistoryLinear.begin(), m_decimHistoryLinear.end(), 0.0f);
    m_iqHistPos = 0;
    m_iqPrevInI = 0.0f;
    m_iqPrevInQ = 0.0f;
    m_iqDcStateI = 0.0f;
    m_iqDcStateQ = 0.0f;
    std::fill(m_iqIHistory.begin(), m_iqIHistory.end(), 0.0f);
    std::fill(m_iqQHistory.begin(), m_iqQHistory.end(), 0.0f);
    m_clipping = false;
    m_clippingRatio = 0.0f;
}

void FMDemod::initAudioFilter() {
    // Default wide channel filter for MPX path.
    rebuildAudioFilter(120000.0);
}

void FMDemod::initIQFilter() {
    // Complex pre-demod channel filtering to reduce adjacent-channel energy.
    rebuildIQFilter(110000.0);
}

void FMDemod::initDecimFilter() {
    constexpr double cutoffHz = 15000.0;
    constexpr double transitionHz = 4000.0;
    int tapCount = static_cast<int>(std::ceil(3.8 * static_cast<double>(m_inputRate) / transitionHz));
    tapCount = std::clamp(tapCount, 63, 1023);
    if ((tapCount % 2) == 0) {
        tapCount++;
    }

    m_decimTaps.assign(static_cast<size_t>(tapCount), 0.0f);
    const int mid = tapCount / 2;
    const double omega = 2.0 * M_PI * cutoffHz / static_cast<double>(m_inputRate);
    double sum = 0.0;
    for (int n = 0; n < tapCount; n++) {
        const int m = n - mid;
        const double sinc = (m == 0)
            ? (omega / M_PI)
            : (std::sin(omega * static_cast<double>(m)) / (M_PI * static_cast<double>(m)));
        const double x = 2.0 * M_PI * static_cast<double>(n) / static_cast<double>(tapCount - 1);
        const double window = 0.355768
                            - 0.487396 * std::cos(x)
                            + 0.144232 * std::cos(2.0 * x)
                            - 0.012604 * std::cos(3.0 * x);
        const double h = sinc * window;
        m_decimTaps[static_cast<size_t>(n)] = static_cast<float>(h);
        sum += h;
    }

    if (std::abs(sum) > 1e-12) {
        const float invSum = static_cast<float>(1.0 / sum);
        for (float& tap : m_decimTaps) {
            tap *= invSum;
        }
    }

    m_decimTapsRev.resize(m_decimTaps.size());
    std::reverse_copy(m_decimTaps.begin(), m_decimTaps.end(), m_decimTapsRev.begin());
    m_decimHistoryLinear.assign(m_decimTaps.size() * 2, 0.0f);
    m_decimHistPos = 0;
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

void FMDemod::rebuildIQFilter(double cutoffHz) {
    constexpr double transitionHz = 16000.0;
    int tapCount = static_cast<int>(std::ceil(3.8 * static_cast<double>(m_inputRate) / transitionHz));
    tapCount = std::clamp(tapCount, 33, 255);
    if ((tapCount % 2) == 0) {
        tapCount++;
    }

    m_iqTaps.assign(static_cast<size_t>(tapCount), 0.0f);
    const int mid = tapCount / 2;
    const double omega = 2.0 * M_PI * cutoffHz / static_cast<double>(m_inputRate);
    double sum = 0.0;
    for (int n = 0; n < tapCount; n++) {
        const int m = n - mid;
        const double sinc = (m == 0)
            ? (omega / M_PI)
            : (std::sin(omega * static_cast<double>(m)) / (M_PI * static_cast<double>(m)));
        const double x = 2.0 * M_PI * static_cast<double>(n) / static_cast<double>(tapCount - 1);
        const double window = 0.355768
                            - 0.487396 * std::cos(x)
                            + 0.144232 * std::cos(2.0 * x)
                            - 0.012604 * std::cos(3.0 * x);
        const double h = sinc * window;
        m_iqTaps[static_cast<size_t>(n)] = static_cast<float>(h);
        sum += h;
    }

    if (std::abs(sum) > 1e-12) {
        const float invSum = static_cast<float>(1.0 / sum);
        for (float& tap : m_iqTaps) {
            tap *= invSum;
        }
    }

    m_iqTapsRev.resize(m_iqTaps.size());
    std::reverse_copy(m_iqTaps.begin(), m_iqTaps.end(), m_iqTapsRev.begin());
    m_iqIHistory.assign(m_iqTaps.size() * 2, 0.0f);
    m_iqQHistory.assign(m_iqTaps.size() * 2, 0.0f);
    m_iqHistPos = 0;
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
    // Keep enough MPX bandwidth for stereo (38 kHz) and RDS (57 kHz).
    const double cutoffHz = (selectedBwHz > 0)
                                ? std::clamp(static_cast<double>(selectedBwHz) * 0.5, 60000.0, 120000.0)
                                : 120000.0;
    rebuildAudioFilter(cutoffHz);
    const double iqCutoffHz = (selectedBwHz > 0)
                                  ? std::clamp(static_cast<double>(selectedBwHz) * 0.45, 90000.0, 120000.0)
                                  : 110000.0;
    rebuildIQFilter(iqCutoffHz);
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
    constexpr float kDcBlockR = 0.9995f;
    const auto& kIqNorm = iqNormLut();
    const uint8_t* iqPtr = iq;
    if (m_iqTaps.empty() || m_iqTapsRev.empty() || m_iqIHistory.empty() || m_iqQHistory.empty()) {
        initIQFilter();
    }
    const size_t iqTapCount = m_iqTaps.size();

    size_t clipCount = 0;

    if (m_demodMode == DemodMode::Fast) {
        float prevI = m_prevI;
        float prevQ = m_prevQ;
        bool havePrev = m_havePrevIQ;
        for (size_t i = 0; i < len; i++) {
            // rtl_tcp provides unsigned 8-bit IQ centered at 127.5.
            const uint8_t iByte = iqPtr[0];
            const uint8_t qByte = iqPtr[1];
            iqPtr += 2;

            // Detect ADC clipping (values at rails 0 or 255 indicate saturation).
            if (iByte == 0 || iByte == 255 || qByte == 0 || qByte == 255) {
                clipCount++;
            }

            const float iRaw = kIqNorm[iByte];
            const float qRaw = kIqNorm[qByte];

            // Remove ADC DC offset and front-end LO bias from raw IQ.
            const float iDc = (iRaw - m_iqPrevInI) + (kDcBlockR * m_iqDcStateI);
            const float qDc = (qRaw - m_iqPrevInQ) + (kDcBlockR * m_iqDcStateQ);
            m_iqPrevInI = iRaw;
            m_iqPrevInQ = qRaw;
            m_iqDcStateI = iDc;
            m_iqDcStateQ = qDc;

            // Pre-demod complex channel filter.
            m_iqIHistory[m_iqHistPos] = iDc;
            m_iqIHistory[m_iqHistPos + iqTapCount] = iDc;
            m_iqQHistory[m_iqHistPos] = qDc;
            m_iqQHistory[m_iqHistPos + iqTapCount] = qDc;
            m_iqHistPos++;
            if (m_iqHistPos >= iqTapCount) {
                m_iqHistPos = 0;
            }

            const float* iWindow = &m_iqIHistory[m_iqHistPos];
            const float* qWindow = &m_iqQHistory[m_iqHistPos];
            float i_val = 0.0f;
            float q_val = 0.0f;
#if FMTUNER_FMDEMOD_HAS_AVX2_KERNEL
            if (m_useAvx2) {
                i_val = firLinearAvx2Fma(m_iqTapsRev.data(), iWindow, iqTapCount);
                q_val = firLinearAvx2Fma(m_iqTapsRev.data(), qWindow, iqTapCount);
            } else
#endif
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
            if (m_useNeon) {
                i_val = firLinearNeon(m_iqTapsRev.data(), iWindow, iqTapCount);
                q_val = firLinearNeon(m_iqTapsRev.data(), qWindow, iqTapCount);
            } else
#endif
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
            if (m_useSse2) {
                i_val = firLinearSse2(m_iqTapsRev.data(), iWindow, iqTapCount);
                q_val = firLinearSse2(m_iqTapsRev.data(), qWindow, iqTapCount);
            } else
#endif
            {
                i_val = firLinearScalar(m_iqTapsRev.data(), iWindow, iqTapCount);
                q_val = firLinearScalar(m_iqTapsRev.data(), qWindow, iqTapCount);
            }

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
            const float power = (i_val * i_val) + (q_val * q_val);
            const float invPower = 1.0f / std::max(power, 1e-6f);
            const float delta = ((i_val * dQ) - (q_val * dI)) * invPower;
            audio[i] = delta * m_invDeviation;
            prevI = i_val;
            prevQ = q_val;
        }
        m_prevI = prevI;
        m_prevQ = prevQ;
        m_havePrevIQ = havePrev;
        m_clipping = (clipCount > 0);
        m_clippingRatio = static_cast<float>(clipCount) / static_cast<float>(len);
        return;
    }

    float lastPhase = m_lastPhase;
    bool havePhase = m_haveLastPhase;
    for (size_t i = 0; i < len; i++) {
        const uint8_t iByte = iqPtr[0];
        const uint8_t qByte = iqPtr[1];
        iqPtr += 2;

        if (iByte == 0 || iByte == 255 || qByte == 0 || qByte == 255) {
            clipCount++;
        }

        const float iRaw = kIqNorm[iByte];
        const float qRaw = kIqNorm[qByte];

        const float iDc = (iRaw - m_iqPrevInI) + (kDcBlockR * m_iqDcStateI);
        const float qDc = (qRaw - m_iqPrevInQ) + (kDcBlockR * m_iqDcStateQ);
        m_iqPrevInI = iRaw;
        m_iqPrevInQ = qRaw;
        m_iqDcStateI = iDc;
        m_iqDcStateQ = qDc;

        m_iqIHistory[m_iqHistPos] = iDc;
        m_iqIHistory[m_iqHistPos + iqTapCount] = iDc;
        m_iqQHistory[m_iqHistPos] = qDc;
        m_iqQHistory[m_iqHistPos + iqTapCount] = qDc;
        m_iqHistPos++;
        if (m_iqHistPos >= iqTapCount) {
            m_iqHistPos = 0;
        }

        const float* iWindow = &m_iqIHistory[m_iqHistPos];
        const float* qWindow = &m_iqQHistory[m_iqHistPos];
        float i_val = 0.0f;
        float q_val = 0.0f;
#if FMTUNER_FMDEMOD_HAS_AVX2_KERNEL
        if (m_useAvx2) {
            i_val = firLinearAvx2Fma(m_iqTapsRev.data(), iWindow, iqTapCount);
            q_val = firLinearAvx2Fma(m_iqTapsRev.data(), qWindow, iqTapCount);
        } else
#endif
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
        if (m_useNeon) {
            i_val = firLinearNeon(m_iqTapsRev.data(), iWindow, iqTapCount);
            q_val = firLinearNeon(m_iqTapsRev.data(), qWindow, iqTapCount);
        } else
#endif
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
        if (m_useSse2) {
            i_val = firLinearSse2(m_iqTapsRev.data(), iWindow, iqTapCount);
            q_val = firLinearSse2(m_iqTapsRev.data(), qWindow, iqTapCount);
        } else
#endif
        {
            i_val = firLinearScalar(m_iqTapsRev.data(), iWindow, iqTapCount);
            q_val = firLinearScalar(m_iqTapsRev.data(), qWindow, iqTapCount);
        }

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
        } else if (delta < -kPi) {
            delta += kTwoPi;
        }
        audio[i] = delta * m_invDeviation;
        lastPhase = phase;
    }
    m_lastPhase = lastPhase;
    m_haveLastPhase = havePhase;

    m_clipping = (clipCount > 0);
    m_clippingRatio = static_cast<float>(clipCount) / static_cast<float>(len);
}

size_t FMDemod::downsampleAudio(const float* demod, float* audio, size_t numSamples) {
    if (m_decimTaps.empty() || m_decimTapsRev.empty() || m_decimHistoryLinear.empty()) {
        return 0;
    }

    const size_t tapCount = m_decimTaps.size();
    size_t outCount = 0;

    for (size_t i = 0; i < numSamples; i++) {
        m_decimHistoryLinear[m_decimHistPos] = demod[i];
        m_decimHistoryLinear[m_decimHistPos + tapCount] = demod[i];
        m_decimHistPos++;
        if (m_decimHistPos >= tapCount) {
            m_decimHistPos = 0;
        }

        m_decimPhase++;
        if (m_decimPhase < m_downsampleFactor) {
            continue;
        }
        m_decimPhase = 0;

        const float* historyWindow = &m_decimHistoryLinear[m_decimHistPos];
        float sample = 0.0f;
#if FMTUNER_FMDEMOD_HAS_AVX2_KERNEL
        if (m_useAvx2) {
            sample = firLinearAvx2Fma(m_decimTapsRev.data(), historyWindow, tapCount);
        } else
#endif
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
        if (m_useNeon) {
            sample = firLinearNeon(m_decimTapsRev.data(), historyWindow, tapCount);
        } else
#endif
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
        if (m_useSse2) {
            sample = firLinearSse2(m_decimTapsRev.data(), historyWindow, tapCount);
        } else
#endif
        {
            sample = firLinearScalar(m_decimTapsRev.data(), historyWindow, tapCount);
        }

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

size_t FMDemod::processSplit(const uint8_t* iq, float* mpxOut, float* monoOut, size_t numSamples) {
    if (m_demodScratch.size() < numSamples) {
        m_demodScratch.resize(numSamples);
    }
    demodulate(iq, m_demodScratch.data(), numSamples);
    if (mpxOut) {
        std::memcpy(mpxOut, m_demodScratch.data(), numSamples * sizeof(float));
    }
    if (!monoOut) {
        return 0;
    }
    return downsampleAudio(m_demodScratch.data(), monoOut, numSamples);
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
        float sample = 0.0f;
#if FMTUNER_FMDEMOD_HAS_AVX2_KERNEL
        if (m_useAvx2) {
            sample = firLinearAvx2Fma(m_audioTapsRev.data(), historyWindow, tapCount);
        } else
#endif
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
        if (m_useNeon) {
            sample = firLinearNeon(m_audioTapsRev.data(), historyWindow, tapCount);
        } else
#endif
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
        if (m_useSse2) {
            sample = firLinearSse2(m_audioTapsRev.data(), historyWindow, tapCount);
        } else
#endif
        {
            sample = firLinearScalar(m_audioTapsRev.data(), historyWindow, tapCount);
        }
        audio[i] = sample;
    }
}
