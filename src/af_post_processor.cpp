#include "af_post_processor.h"

#include <algorithm>
#include <cmath>

AFPostProcessor::AFPostProcessor(int inputRate, int outputRate)
    : m_inputRate(std::max(1, inputRate))
    , m_outputRate(std::max(1, outputRate))
    , m_resampleStep(static_cast<double>(std::max(1, inputRate)) /
                     static_cast<double>(std::max(1, outputRate)))
    , m_resamplePos(0.0)
    , m_deemphasisEnabled(true)
    , m_deemphAlpha(1.0f)
    , m_deemphStateLeft(0.0f)
    , m_deemphStateRight(0.0f) {
    setDeemphasis(75);
}

void AFPostProcessor::reset() {
    m_resamplePos = 0.0;
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

    size_t outCount = 0;
    double pos = m_resamplePos;

    while (pos < static_cast<double>(inSamples) && outCount < outCapacity) {
        const size_t i0 = static_cast<size_t>(pos);
        const size_t i1 = std::min(i0 + 1, inSamples - 1);
        const float frac = static_cast<float>(pos - static_cast<double>(i0));

        float left = inLeft[i0] + ((inLeft[i1] - inLeft[i0]) * frac);
        float right = inRight[i0] + ((inRight[i1] - inRight[i0]) * frac);

        if (m_deemphasisEnabled) {
            m_deemphStateLeft = (m_deemphAlpha * left) + ((1.0f - m_deemphAlpha) * m_deemphStateLeft);
            m_deemphStateRight = (m_deemphAlpha * right) + ((1.0f - m_deemphAlpha) * m_deemphStateRight);
            left = m_deemphStateLeft;
            right = m_deemphStateRight;
        }

        outLeft[outCount] = std::clamp(left, -1.0f, 1.0f);
        outRight[outCount] = std::clamp(right, -1.0f, 1.0f);
        outCount++;
        pos += m_resampleStep;
    }

    m_resamplePos = pos - static_cast<double>(inSamples);
    if (m_resamplePos < 0.0) {
        m_resamplePos = 0.0;
    }
    return outCount;
}
