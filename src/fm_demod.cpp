#include "fm_demod.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>

namespace {
constexpr float kPi = 3.14159265358979323846f;
constexpr std::array<int, 30> kXdrFmBwHz = {
    309000, 298000, 281000, 263000, 246000, 229000, 211000, 194000,
    177000, 159000, 142000, 125000, 108000, 95000, 90000, 83000,
    73000, 63000, 55000, 48000, 42000, 36000, 32000, 27000,
    24000, 20000, 17000, 15000, 9000, 0
};

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
    , m_deviation(75000.0)
    , m_deemphasisEnabled(true)
    , m_bandwidthMode(0)
    , m_w0BandwidthHz(194000)
    , m_clipping(false)
    , m_clippingRatio(0.0f) {
    const float iqCutoffNorm = std::clamp(110000.0f / static_cast<float>(m_inputRate), 0.01f, 0.45f);
    m_liquidIqFilter.init(81, iqCutoffNorm);
    m_liquidIqDcBlockI.initDCBlocker(0.0005f);
    m_liquidIqDcBlockQ.initDCBlocker(0.0005f);
    const float ratio = static_cast<float>(m_outputRate) / static_cast<float>(m_inputRate);
    m_liquidMonoResampler.init(ratio);
    m_liquidMonoDcBlock.initDCBlocker(0.0008f);
    setDeviation(75000.0);
    setDeemphasis(75);
}

FMDemod::~FMDemod() = default;

void FMDemod::setDeemphasis(int tau_us) {
    if (tau_us <= 0) {
        m_deemphasisEnabled = false;
        return;
    }
    m_deemphasisEnabled = true;
    const float tau = static_cast<float>(tau_us) * 1e-6f;
    const float dt = 1.0f / static_cast<float>(m_outputRate);
    const float alpha = dt / (tau + dt);
    const std::vector<float> b = {alpha};
    const std::vector<float> a = {1.0f, -(1.0f - alpha)};
    m_liquidMonoDeemphasis.init(b, a);
}

void FMDemod::setDeviation(double deviation) {
    m_deviation = deviation;
    // liquid freqdem expects kf in cycles/sample; match legacy gain:
    // out = delta_phase * Fs / (2*pi*deviation)
    // => kf = deviation / Fs
    m_liquidFreqDemod.init(static_cast<float>(m_deviation / static_cast<double>(m_inputRate)));
}

void FMDemod::reset() {
    m_clipping = false;
    m_clippingRatio = 0.0f;
    m_liquidIqFilter.reset();
    m_liquidFreqDemod.reset();
    m_liquidIqDcBlockI.reset();
    m_liquidIqDcBlockQ.reset();
    if (m_deemphasisEnabled) {
        m_liquidMonoDeemphasis.reset();
    }
    m_liquidMonoDcBlock.reset();
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
    const int effectiveBwHz = (bwHz <= 0) ? m_w0BandwidthHz : bwHz;
    int selected = static_cast<int>(kXdrFmBwHz.size() - 1);
    if (effectiveBwHz > 0) {
        int minDiff = std::numeric_limits<int>::max();
        for (int i = 0; i < static_cast<int>(kXdrFmBwHz.size()) - 1; i++) {
            const int diff = std::abs(kXdrFmBwHz[static_cast<size_t>(i)] - effectiveBwHz);
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
    const int selectedBwHz = kXdrFmBwHz[static_cast<size_t>(selected)];
    // Parameterize liquid LPF from the mapped RF bandwidth:
    // channel BW -> baseband half-band cutoff, clamped to available Nyquist headroom.
    const double nyquistHeadroomHz = 0.45 * static_cast<double>(m_inputRate);
    const double iqCutoffHz = (selectedBwHz > 0)
        ? std::clamp(static_cast<double>(selectedBwHz) * 0.5, 9000.0, nyquistHeadroomHz)
        : nyquistHeadroomHz;
    const float cutoffNorm = std::clamp(static_cast<float>(iqCutoffHz / static_cast<double>(m_inputRate)), 0.01f, 0.45f);
    const std::uint32_t filterLen = (selectedBwHz > 0 && selectedBwHz <= 73000) ? 121U : 81U;
    const float stopBandAtten = (selectedBwHz > 0 && selectedBwHz <= 42000) ? 70.0f : 60.0f;
    m_liquidIqFilter.init(filterLen, cutoffNorm, stopBandAtten);
}

void FMDemod::setW0BandwidthHz(int bwHz) {
    m_w0BandwidthHz = std::clamp(bwHz, 0, 400000);
}

void FMDemod::demodulate(const uint8_t* iq, float* audio, size_t len) {
    const auto& kIqNorm = iqNormLut();
    const uint8_t* iqPtr = iq;
    size_t clipCount = 0;

    for (size_t i = 0; i < len; i++) {
        const uint8_t iByte = iqPtr[0];
        const uint8_t qByte = iqPtr[1];
        iqPtr += 2;
        if (iByte == 0 || iByte == 255 || qByte == 0 || qByte == 255) {
            clipCount++;
        }

        const float iRaw = kIqNorm[iByte];
        const float qRaw = kIqNorm[qByte];
        const float iDc = m_liquidIqDcBlockI.execute(iRaw);
        const float qDc = m_liquidIqDcBlockQ.execute(qRaw);

        m_liquidIqFilter.push(std::complex<float>(iDc, qDc));
        const std::complex<float> iqFilt = m_liquidIqFilter.execute();
        audio[i] = m_liquidFreqDemod.execute(iqFilt);
    }

    m_clipping = (clipCount > 0);
    m_clippingRatio = (len > 0) ? (static_cast<float>(clipCount) / static_cast<float>(len)) : 0.0f;
}

size_t FMDemod::downsampleAudio(const float* demod, float* audio, size_t numSamples) {
    size_t outCount = 0;
    for (size_t i = 0; i < numSamples; i++) {
        const uint32_t produced = m_liquidMonoResampler.execute(demod[i], m_liquidResampleTmp);
        for (uint32_t p = 0; p < produced; p++) {
            float sample = m_liquidResampleTmp[p];
            if (m_deemphasisEnabled) {
                sample = m_liquidMonoDeemphasis.execute(sample);
            }
            sample = m_liquidMonoDcBlock.execute(sample);
            audio[outCount++] = sample;
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
