#include "fm_demod.h"
#include <cmath>
#include <cstring>
#include <algorithm>

FMDemod::FMDemod(int inputRate, int outputRate)
    : m_inputRate(inputRate)
    , m_outputRate(outputRate)
    , m_downsampleFactor(inputRate / outputRate)
    , m_oversample(1)
    , m_firBuffer(nullptr)
    , m_firLen(0)
    , m_lastPhase(0)
    , m_deviation(75000.0)
    , m_invDeviation(0.0)
    , m_deemphA(0)
    , m_deemphasisState(0) {

    setDeviation(75000.0);
    setDeemphasis(75);
}

FMDemod::~FMDemod() {
    delete[] m_firBuffer;
}

void FMDemod::setDeemphasis(int tau_us) {
    m_deemphA = (int)std::round(1.0 / (1.0 - std::exp(-1.0 / (m_outputRate * tau_us * 1e-6))));
}

void FMDemod::setOversample(int os) {
    m_oversample = os;
}

void FMDemod::setDeviation(double deviation) {
    m_deviation = deviation;
    m_invDeviation = m_inputRate / (2.0 * M_PI * deviation);
}

void FMDemod::reset() {
    m_lastPhase = 0;
    m_deemphasisState = 0;
    if (m_firBuffer) {
        memset(m_firBuffer, 0, m_firLen * sizeof(float));
    }
}

void FMDemod::downsample(const float* input, float* output, size_t inputLen, size_t* outputLen) {
    size_t outLen = inputLen / m_downsampleFactor;
    *outputLen = outLen;

    for (size_t i = 0; i < outLen; i++) {
        output[i] = input[i * m_downsampleFactor];
    }
}

void FMDemod::demodulate(const float* iq, float* audio, size_t len) {
    for (size_t i = 0; i < len; i++) {
        float i_val = iq[i * 2];
        float q_val = iq[i * 2 + 1];

        float phase = std::atan2(q_val, i_val);
        float delta = phase - m_lastPhase;

        while (delta > M_PI) delta -= 2.0f * M_PI;
        while (delta <= -M_PI) delta += 2.0f * M_PI;

        audio[i] = delta * m_invDeviation;
        m_lastPhase = phase;
    }
}

void FMDemod::deemphasize(float* audio, size_t len) {
    for (size_t i = 0; i < len; i++) {
        int d = (int)(audio[i] * 32767.0f - m_deemphasisState);
        if (d > 0) {
            m_deemphasisState += (d + m_deemphA / 2) / m_deemphA;
        } else {
            m_deemphasisState += (d - m_deemphA / 2) / m_deemphA;
        }
        audio[i] = m_deemphasisState / 32767.0f;
    }
}

void FMDemod::process(const int8_t* iq, float* audio, size_t numSamples) {
    float* floatBuf = new float[numSamples * 2];
    float* demodulated = new float[numSamples];

    for (size_t i = 0; i < numSamples * 2; i++) {
        floatBuf[i] = iq[i] / 128.0f;
    }

    demodulate(floatBuf, demodulated, numSamples);

    deemphasize(demodulated, numSamples);

    size_t downLen = 0;
    downsample(demodulated, audio, numSamples, &downLen);

    delete[] floatBuf;
    delete[] demodulated;
}

void FMDemod::processNoDownsample(const int8_t* iq, float* audio, size_t numSamples) {
    float* floatBuf = new float[numSamples * 2];
    float* demodulated = new float[numSamples];

    for (size_t i = 0; i < numSamples * 2; i++) {
        floatBuf[i] = iq[i] / 128.0f;
    }

    demodulate(floatBuf, demodulated, numSamples);

    deemphasize(demodulated, numSamples);

    for (size_t i = 0; i < numSamples; i++) {
        audio[i] = demodulated[i];
    }

    delete[] floatBuf;
    delete[] demodulated;
}
