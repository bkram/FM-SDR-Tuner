#include "af_post_processor.h"
#include "cpu_features.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
#include <immintrin.h>
#endif
#if defined(__aarch64__) || defined(__arm__) || defined(_M_ARM64)
#include <arm_neon.h>
#endif

namespace {
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

AFPostProcessor::AFPostProcessor(int inputRate, int outputRate)
    : m_inputRate(std::max(1, inputRate))
    , m_outputRate(std::max(1, outputRate))
    , m_upFactor(1)
    , m_downFactor(1)
    , m_halfTaps(32)
    , m_tapCount((m_halfTaps * 2) + 1)
    , m_bufferStart(0)
    , m_timeAcc(0)
    , m_useNeon(false)
    , m_useSse2(false)
    , m_useAvx2(false)
    , m_deemphasisEnabled(true)
    , m_deemphAlpha(1.0f)
    , m_deemphStateLeft(0.0f)
    , m_deemphStateRight(0.0f) {
    const int g = std::gcd(m_inputRate, m_outputRate);
    m_upFactor = std::max(1, m_outputRate / g);
    m_downFactor = std::max(1, m_inputRate / g);
    const CPUFeatures cpu = detectCPUFeatures();
    m_useNeon = cpu.neon;
    m_useSse2 = cpu.sse2;
    m_useAvx2 = cpu.avx2 && cpu.fma;
    designPolyphaseKernel();
    reset();
    setDeemphasis(75);
}

float AFPostProcessor::sinc(float x) const {
    if (std::abs(x) < 1e-8f) {
        return 1.0f;
    }
    const float px = static_cast<float>(M_PI) * x;
    return std::sin(px) / px;
}

void AFPostProcessor::designPolyphaseKernel() {
    m_phaseTaps.assign(static_cast<size_t>(m_upFactor * m_tapCount), 0.0f);
    const float cutoffRatio = std::min(1.0f,
                                       static_cast<float>(m_outputRate) /
                                       static_cast<float>(m_inputRate));

    for (int phase = 0; phase < m_upFactor; phase++) {
        const float frac = static_cast<float>(phase) / static_cast<float>(m_upFactor);
        float sum = 0.0f;

        for (int tap = 0; tap < m_tapCount; tap++) {
            const int rel = tap - m_halfTaps;
            const float x = static_cast<float>(rel) - frac;
            // Kaiser-like quality via windowed sinc. Cutoff set by resampling ratio.
            const float h = cutoffRatio * sinc(cutoffRatio * x);
            const float w = 0.54f - (0.46f * std::cos((2.0f * static_cast<float>(M_PI) * tap) /
                                                      static_cast<float>(m_tapCount - 1)));
            const float coeff = h * w;
            m_phaseTaps[static_cast<size_t>(phase * m_tapCount + tap)] = coeff;
            sum += coeff;
        }

        if (std::abs(sum) > 1e-8f) {
            const float inv = 1.0f / sum;
            for (int tap = 0; tap < m_tapCount; tap++) {
                m_phaseTaps[static_cast<size_t>(phase * m_tapCount + tap)] *= inv;
            }
        }
    }
}

float AFPostProcessor::convolveDot(const float* samples, const float* taps, size_t count) const {
#if defined(__aarch64__) || defined(__arm__) || defined(_M_ARM64)
    if (m_useNeon) {
        return dotProductNeon(samples, taps, count);
    }
#endif
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
#if defined(__AVX2__) && defined(__FMA__)
    if (m_useAvx2) {
        return dotProductAvx2Fma(samples, taps, count);
    }
#endif
    if (m_useSse2) {
        return dotProductSse(samples, taps, count);
    }
#endif
    return dotProductScalar(samples, taps, count);
}

void AFPostProcessor::reset() {
    m_leftBuffer.assign(static_cast<size_t>(m_halfTaps), 0.0f);
    m_rightBuffer.assign(static_cast<size_t>(m_halfTaps), 0.0f);
    m_bufferStart = 0;
    m_timeAcc = static_cast<int64_t>(m_halfTaps) * static_cast<int64_t>(m_upFactor);
    m_deemphStateLeft = 0.0f;
    m_deemphStateRight = 0.0f;
}

void AFPostProcessor::setDeemphasis(int tau_us) {
    if (tau_us <= 0) {
        m_deemphasisEnabled = false;
        m_deemphAlpha = 1.0f;
        return;
    }

    m_deemphasisEnabled = true;
    const float tau = static_cast<float>(tau_us) * 1e-6f;
    const float dt = 1.0f / static_cast<float>(m_outputRate);
    m_deemphAlpha = dt / (tau + dt);
}

size_t AFPostProcessor::process(const float* inLeft,
                                const float* inRight,
                                size_t inSamples,
                                float* outLeft,
                                float* outRight,
                                size_t outCapacity) {
    if (!inLeft || !inRight || !outLeft || !outRight || inSamples == 0 || outCapacity == 0) {
        return 0;
    }

    const size_t oldSize = m_leftBuffer.size();
    m_leftBuffer.resize(oldSize + inSamples);
    m_rightBuffer.resize(oldSize + inSamples);
    std::memcpy(m_leftBuffer.data() + oldSize, inLeft, inSamples * sizeof(float));
    std::memcpy(m_rightBuffer.data() + oldSize, inRight, inSamples * sizeof(float));

    size_t outCount = 0;
    size_t available = m_leftBuffer.size() - m_bufferStart;
    while (outCount < outCapacity) {
        const int64_t center = m_timeAcc / static_cast<int64_t>(m_upFactor);
        const int phase = static_cast<int>(m_timeAcc % static_cast<int64_t>(m_upFactor));
        const int64_t start = center - static_cast<int64_t>(m_halfTaps);
        const int64_t end = start + static_cast<int64_t>(m_tapCount);
        if (start < 0 || end > static_cast<int64_t>(available)) {
            break;
        }

        const float* taps = &m_phaseTaps[static_cast<size_t>(phase * m_tapCount)];
        const size_t idx = m_bufferStart + static_cast<size_t>(start);
        float left = convolveDot(m_leftBuffer.data() + idx, taps, static_cast<size_t>(m_tapCount));
        float right = convolveDot(m_rightBuffer.data() + idx, taps, static_cast<size_t>(m_tapCount));

        if (m_deemphasisEnabled) {
            m_deemphStateLeft = (m_deemphAlpha * left) + ((1.0f - m_deemphAlpha) * m_deemphStateLeft);
            m_deemphStateRight = (m_deemphAlpha * right) + ((1.0f - m_deemphAlpha) * m_deemphStateRight);
            left = m_deemphStateLeft;
            right = m_deemphStateRight;
        }

        outLeft[outCount] = std::clamp(left, -1.0f, 1.0f);
        outRight[outCount] = std::clamp(right, -1.0f, 1.0f);
        outCount++;
        m_timeAcc += static_cast<int64_t>(m_downFactor);
    }

    const int64_t center = m_timeAcc / static_cast<int64_t>(m_upFactor);
    const int64_t keepFrom = std::max<int64_t>(0, center - static_cast<int64_t>(m_halfTaps));
    if (keepFrom > 0) {
        available = m_leftBuffer.size() - m_bufferStart;
        const size_t eraseCount = static_cast<size_t>(std::min<int64_t>(keepFrom, static_cast<int64_t>(available)));
        m_bufferStart += eraseCount;
        m_timeAcc -= static_cast<int64_t>(eraseCount) * static_cast<int64_t>(m_upFactor);
    }

    available = m_leftBuffer.size() - m_bufferStart;
    if (m_bufferStart > 0 && (m_bufferStart >= 32768 || m_bufferStart > available)) {
        std::memmove(m_leftBuffer.data(), m_leftBuffer.data() + m_bufferStart, available * sizeof(float));
        std::memmove(m_rightBuffer.data(), m_rightBuffer.data() + m_bufferStart, available * sizeof(float));
        m_leftBuffer.resize(available);
        m_rightBuffer.resize(available);
        m_bufferStart = 0;
    }

    return outCount;
}
