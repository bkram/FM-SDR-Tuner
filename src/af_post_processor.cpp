#include "af_post_processor.h"

#include <algorithm>
#include <cmath>

AFPostProcessor::AFPostProcessor(int inputRate, int outputRate)
    : m_inputRate(std::max(1, inputRate))
    , m_outputRate(std::max(1, outputRate))
    , m_deemphasisEnabled(true)
    , m_deemphAlpha(1.0f)
    , m_deemphStateLeft(0.0f)
    , m_deemphStateRight(0.0f)
    , m_dcBlockPrevInLeft(0.0f)
    , m_dcBlockPrevInRight(0.0f)
    , m_dcBlockPrevOutLeft(0.0f)
    , m_dcBlockPrevOutRight(0.0f) {
    const float ratio = static_cast<float>(m_outputRate) / static_cast<float>(m_inputRate);
    m_liquidLeftResampler.init(ratio);
    m_liquidRightResampler.init(ratio);
    reset();
    setDeemphasis(75);
}

void AFPostProcessor::reset() {
    m_deemphStateLeft = 0.0f;
    m_deemphStateRight = 0.0f;
    m_dcBlockPrevInLeft = 0.0f;
    m_dcBlockPrevInRight = 0.0f;
    m_dcBlockPrevOutLeft = 0.0f;
    m_dcBlockPrevOutRight = 0.0f;
    m_liquidLeftResampler.reset();
    m_liquidRightResampler.reset();
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
    for (size_t i = 0; i < inSamples && outCount < outCapacity; i++) {
        const uint32_t leftProduced = m_liquidLeftResampler.execute(inLeft[i], m_liquidLeftTmp);
        const uint32_t rightProduced = m_liquidRightResampler.execute(inRight[i], m_liquidRightTmp);
        const uint32_t produced = std::min(leftProduced, rightProduced);

        for (uint32_t p = 0; p < produced && outCount < outCapacity; p++) {
            float left = m_liquidLeftTmp[p];
            float right = m_liquidRightTmp[p];
            if (m_deemphasisEnabled) {
                m_deemphStateLeft = (m_deemphAlpha * left) + ((1.0f - m_deemphAlpha) * m_deemphStateLeft);
                m_deemphStateRight = (m_deemphAlpha * right) + ((1.0f - m_deemphAlpha) * m_deemphStateRight);
                left = m_deemphStateLeft;
                right = m_deemphStateRight;
            }
            outLeft[outCount] = left;
            outRight[outCount] = right;
            outCount++;
        }
    }

    processDCBlock(outLeft, outRight, outCount);
    return outCount;
}

void AFPostProcessor::processDCBlock(float* left, float* right, size_t samples) {
    for (size_t i = 0; i < samples; i++) {
        const float inL = left[i];
        const float inR = right[i];

        const float outL = (inL - m_dcBlockPrevInLeft) + (kDcBlockR * m_dcBlockPrevOutLeft);
        const float outR = (inR - m_dcBlockPrevInRight) + (kDcBlockR * m_dcBlockPrevOutRight);

        m_dcBlockPrevInLeft = inL;
        m_dcBlockPrevInRight = inR;
        m_dcBlockPrevOutLeft = outL;
        m_dcBlockPrevOutRight = outR;

        left[i] = outL;
        right[i] = outR;
    }
}
