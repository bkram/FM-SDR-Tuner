#include "fm_demod.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>

namespace {
constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = 6.28318530717958647692f;

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
}  // namespace

FMDemod::FMDemod(int inputRate, int outputRate)
    : m_inputRate(std::max(1, inputRate))
    , m_outputRate(std::max(1, outputRate))
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
    , m_iqPrevInI(0.0f)
    , m_iqPrevInQ(0.0f)
    , m_iqDcStateI(0.0f)
    , m_iqDcStateQ(0.0f)
    , m_clipping(false)
    , m_clippingRatio(0.0f) {
    setDeviation(75000.0);
    setDeemphasis(75);
    const float iqCutoffNorm = std::clamp(110000.0f / static_cast<float>(m_inputRate), 0.01f, 0.45f);
    m_liquidIqFilter.init(81, iqCutoffNorm);
    const float ratio = static_cast<float>(m_outputRate) / static_cast<float>(m_inputRate);
    m_liquidMonoResampler.init(ratio);
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
    m_invDeviation = m_inputRate / (2.0 * static_cast<double>(kPi) * deviation);
}

void FMDemod::reset() {
    m_lastPhase = 0.0f;
    m_haveLastPhase = false;
    m_prevI = 0.0f;
    m_prevQ = 0.0f;
    m_havePrevIQ = false;
    m_deemphasisState = 0.0f;
    m_iqPrevInI = 0.0f;
    m_iqPrevInQ = 0.0f;
    m_iqDcStateI = 0.0f;
    m_iqDcStateQ = 0.0f;
    m_clipping = false;
    m_clippingRatio = 0.0f;
    m_liquidIqFilter.reset();
    m_liquidMonoResampler.reset();
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
    static constexpr int kXdrFmBwHz[] = {
        309000, 298000, 281000, 263000, 246000, 229000, 211000, 194000,
        177000, 159000, 142000, 125000, 108000, 95000, 90000, 83000,
        73000, 63000, 55000, 48000, 42000, 36000, 32000, 27000,
        24000, 20000, 17000, 15000, 9000, 0
    };

    int selected = static_cast<int>(std::size(kXdrFmBwHz) - 1);
    if (bwHz > 0) {
        int minDiff = std::numeric_limits<int>::max();
        for (int i = 0; i < static_cast<int>(std::size(kXdrFmBwHz)) - 1; i++) {
            const int diff = std::abs(kXdrFmBwHz[i] - bwHz);
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
    const int selectedBwHz = kXdrFmBwHz[selected];
    const double iqCutoffHz = (selectedBwHz > 0)
                                  ? std::clamp(static_cast<double>(selectedBwHz) * 0.70, 15000.0, 120000.0)
                                  : 110000.0;
    const float cutoffNorm = std::clamp(static_cast<float>(iqCutoffHz / static_cast<double>(m_inputRate)), 0.01f, 0.45f);
    m_liquidIqFilter.init(81, cutoffNorm);
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
    constexpr float kDcBlockR = 0.9995f;
    const auto& kIqNorm = iqNormLut();
    const uint8_t* iqPtr = iq;
    size_t clipCount = 0;

    if (m_demodMode == DemodMode::Fast) {
        float prevI = m_prevI;
        float prevQ = m_prevQ;
        bool havePrev = m_havePrevIQ;
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

            m_liquidIqFilter.push(std::complex<float>(iDc, qDc));
            const std::complex<float> iqFilt = m_liquidIqFilter.execute();
            const float iVal = iqFilt.real();
            const float qVal = iqFilt.imag();

            if (!havePrev) {
                audio[i] = 0.0f;
                prevI = iVal;
                prevQ = qVal;
                havePrev = true;
                continue;
            }

            const float dI = iVal - prevI;
            const float dQ = qVal - prevQ;
            const float power = (iVal * iVal) + (qVal * qVal);
            const float invPower = 1.0f / std::max(power, 1e-6f);
            const float delta = ((iVal * dQ) - (qVal * dI)) * invPower;
            audio[i] = delta * static_cast<float>(m_invDeviation);
            prevI = iVal;
            prevQ = qVal;
        }
        m_prevI = prevI;
        m_prevQ = prevQ;
        m_havePrevIQ = havePrev;
    } else {
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

            m_liquidIqFilter.push(std::complex<float>(iDc, qDc));
            const std::complex<float> iqFilt = m_liquidIqFilter.execute();
            const float phase = atan2f(iqFilt.imag(), iqFilt.real());

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
            audio[i] = delta * static_cast<float>(m_invDeviation);
            lastPhase = phase;
        }
        m_lastPhase = lastPhase;
        m_haveLastPhase = havePhase;
    }

    m_clipping = (clipCount > 0);
    m_clippingRatio = (len > 0) ? (static_cast<float>(clipCount) / static_cast<float>(len)) : 0.0f;
}

size_t FMDemod::downsampleAudio(const float* demod, float* audio, size_t numSamples) {
    size_t outCount = 0;
    for (size_t i = 0; i < numSamples; i++) {
        const uint32_t produced = m_liquidMonoResampler.execute(demod[i], m_liquidResampleTmp);
        for (uint32_t p = 0; p < produced; p++) {
            const float sample = m_liquidResampleTmp[p];
            m_deemphasisState = (m_deemphAlpha * sample) + ((1.0f - m_deemphAlpha) * m_deemphasisState);
            audio[outCount++] = m_deemphasisState;
        }
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
    demodulate(iq, audio, numSamples);
}
