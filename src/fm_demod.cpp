#include "fm_demod.h"
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
inline size_t prevCircular(size_t idx, size_t size) {
    return (idx == 0) ? (size - 1) : (idx - 1);
}

float firCircularScalar(const std::vector<float>& taps,
                        const std::vector<float>& history,
                        size_t newestIndex) {
    const size_t tapCount = taps.size();
    float sample = 0.0f;
    size_t histIndex = newestIndex;
    for (size_t t = 0; t < tapCount; t++) {
        sample += taps[t] * history[histIndex];
        histIndex = prevCircular(histIndex, tapCount);
    }
    return sample;
}

#if defined(__AVX2__) && defined(__FMA__)
float firCircularAvx2Fma(const std::vector<float>& taps,
                         const std::vector<float>& history,
                         size_t newestIndex) {
    const size_t tapCount = taps.size();
    __m256 acc = _mm256_setzero_ps();
    size_t histIndex = newestIndex;
    size_t t = 0;

    for (; t + 8 <= tapCount; t += 8) {
        alignas(32) int idxArr[8];
        for (int lane = 0; lane < 8; lane++) {
            idxArr[lane] = static_cast<int>(histIndex);
            histIndex = prevCircular(histIndex, tapCount);
        }

        const __m256 tapVec = _mm256_loadu_ps(&taps[t]);
        const __m256i idxVec = _mm256_load_si256(reinterpret_cast<const __m256i*>(idxArr));
        const __m256 histVec = _mm256_i32gather_ps(history.data(), idxVec, 4);
        acc = _mm256_fmadd_ps(tapVec, histVec, acc);
    }

    alignas(32) float sumArr[8];
    _mm256_store_ps(sumArr, acc);
    float sum = sumArr[0] + sumArr[1] + sumArr[2] + sumArr[3]
              + sumArr[4] + sumArr[5] + sumArr[6] + sumArr[7];

    for (; t < tapCount; t++) {
        sum += taps[t] * history[histIndex];
        histIndex = prevCircular(histIndex, tapCount);
    }
    return sum;
}
#endif

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
float firCircularNeon(const std::vector<float>& taps,
                      const std::vector<float>& history,
                      size_t newestIndex) {
    const size_t tapCount = taps.size();
    float32x4_t acc = vdupq_n_f32(0.0f);
    size_t histIndex = newestIndex;
    size_t t = 0;

    for (; t + 4 <= tapCount; t += 4) {
        float histArr[4];
        for (int lane = 0; lane < 4; lane++) {
            histArr[lane] = history[histIndex];
            histIndex = prevCircular(histIndex, tapCount);
        }

        const float32x4_t tapVec = vld1q_f32(&taps[t]);
        const float32x4_t histVec = vld1q_f32(histArr);
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
        sum += taps[t] * history[histIndex];
        histIndex = prevCircular(histIndex, tapCount);
    }
    return sum;
}
#endif
}  // namespace

FMDemod::FMDemod(int inputRate, int outputRate)
    : m_inputRate(inputRate)
    , m_outputRate(outputRate)
    , m_downsampleFactor(std::max(1, inputRate / outputRate))
    , m_lastPhase(0)
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
    m_deemphasisState = 0.0f;
    m_audioHistPos = 0;
    m_decimPhase = 0;
    std::fill(m_audioHistory.begin(), m_audioHistory.end(), 0.0f);
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

    m_audioHistory.assign(m_audioTaps.size(), 0.0f);
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

void FMDemod::demodulate(const uint8_t* iq, float* audio, size_t len) {
    constexpr float kPi = 3.14159265358979323846f;
    constexpr float kTwoPi = 6.28318530717958647692f;

    for (size_t i = 0; i < len; i++) {
        // rtl_tcp provides unsigned 8-bit IQ centered at 127.5.
        const size_t iqIndex = i * 2;
        const float i_val = (static_cast<float>(iq[iqIndex]) - 127.5f) / 127.5f;
        const float q_val = (static_cast<float>(iq[iqIndex + 1]) - 127.5f) / 127.5f;

        const float phase = std::atan2f(q_val, i_val);
        float delta = phase - m_lastPhase;

        if (delta > kPi) {
            delta -= kTwoPi;
        } else if (delta <= -kPi) {
            delta += kTwoPi;
        }

        audio[i] = delta * m_invDeviation;
        m_lastPhase = phase;
    }
}

size_t FMDemod::downsampleAudio(const float* demod, float* audio, size_t numSamples) {
    if (m_audioTaps.empty() || m_audioHistory.empty()) {
        return 0;
    }

    const size_t tapCount = m_audioTaps.size();
    size_t outCount = 0;

    for (size_t i = 0; i < numSamples; i++) {
        m_audioHistory[m_audioHistPos] = demod[i];
        m_audioHistPos = (m_audioHistPos + 1) % tapCount;

        m_decimPhase++;
        if (m_decimPhase < m_downsampleFactor) {
            continue;
        }
        m_decimPhase = 0;

        float sample = 0.0f;
        size_t histIndex = m_audioHistPos;
        for (size_t t = 0; t < tapCount; t++) {
            histIndex = (histIndex == 0) ? (tapCount - 1) : (histIndex - 1);
            sample += m_audioTaps[t] * m_audioHistory[histIndex];
        }

        // SDR++ applies deemphasis in post-processing after AF resampling.
        m_deemphasisState = (m_deemphAlpha * sample) + ((1.0f - m_deemphAlpha) * m_deemphasisState);
        audio[outCount++] = m_deemphasisState;
    }

    return outCount;
}

void FMDemod::process(const uint8_t* iq, float* audio, size_t numSamples) {
    std::vector<float> demodulated(numSamples);
    demodulate(iq, demodulated.data(), numSamples);
    downsampleAudio(demodulated.data(), audio, numSamples);
}

void FMDemod::processNoDownsample(const uint8_t* iq, float* audio, size_t numSamples) {
    if (m_audioTaps.empty() || m_audioHistory.empty()) {
        demodulate(iq, audio, numSamples);
        return;
    }

    if (m_demodScratch.size() < numSamples) {
        m_demodScratch.resize(numSamples);
    }
    demodulate(iq, m_demodScratch.data(), numSamples);

    const size_t tapCount = m_audioTaps.size();
    for (size_t i = 0; i < numSamples; i++) {
        m_audioHistory[m_audioHistPos] = m_demodScratch[i];
        m_audioHistPos = (m_audioHistPos + 1) % tapCount;

        const size_t newestIndex = prevCircular(m_audioHistPos, tapCount);
#if defined(__AVX2__) && defined(__FMA__)
        const float sample = firCircularAvx2Fma(m_audioTaps, m_audioHistory, newestIndex);
#elif defined(__ARM_NEON) || defined(__ARM_NEON__)
        const float sample = firCircularNeon(m_audioTaps, m_audioHistory, newestIndex);
#else
        const float sample = firCircularScalar(m_audioTaps, m_audioHistory, newestIndex);
#endif
        audio[i] = sample;
    }
}
