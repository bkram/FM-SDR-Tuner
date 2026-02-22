#ifndef AF_POST_PROCESSOR_H
#define AF_POST_PROCESSOR_H

#include <stddef.h>
#include <stdint.h>
#include <array>
#include "dsp/liquid_primitives.h"

class AFPostProcessor {
public:
    AFPostProcessor(int inputRate, int outputRate);

    void reset();
    void setDeemphasis(int tau_us);

    size_t process(const float* inLeft,
                   const float* inRight,
                   size_t inSamples,
                   float* outLeft,
                   float* outRight,
                   size_t outCapacity);

private:
    void processDCBlock(float* left, float* right, size_t samples);

    int m_inputRate;
    int m_outputRate;

    bool m_deemphasisEnabled;
    float m_deemphAlpha;
    float m_deemphStateLeft;
    float m_deemphStateRight;

    float m_dcBlockPrevInLeft;
    float m_dcBlockPrevInRight;
    float m_dcBlockPrevOutLeft;
    float m_dcBlockPrevOutRight;
    static constexpr float kDcBlockR = 0.995f;
    fm_tuner::dsp::liquid::Resampler m_liquidLeftResampler;
    fm_tuner::dsp::liquid::Resampler m_liquidRightResampler;
    std::array<float, fm_tuner::dsp::liquid::Resampler::kMaxOutput> m_liquidLeftTmp{};
    std::array<float, fm_tuner::dsp::liquid::Resampler::kMaxOutput> m_liquidRightTmp{};
};

#endif
