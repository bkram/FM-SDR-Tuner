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
    int m_inputRate;
    int m_outputRate;

    bool m_deemphasisEnabled;
    static constexpr float kDcBlockAlpha = 0.005f;
    static constexpr int kDefaultDeemphasisUs = 75;
    static constexpr float kMicrosecondsToSeconds = 1e-6f;
    fm_tuner::dsp::liquid::IIRFilterReal m_liquidLeftDeemphasis;
    fm_tuner::dsp::liquid::IIRFilterReal m_liquidRightDeemphasis;
    fm_tuner::dsp::liquid::IIRFilterReal m_liquidLeftDcBlock;
    fm_tuner::dsp::liquid::IIRFilterReal m_liquidRightDcBlock;
    fm_tuner::dsp::liquid::Resampler m_liquidLeftResampler;
    fm_tuner::dsp::liquid::Resampler m_liquidRightResampler;
    std::array<float, fm_tuner::dsp::liquid::Resampler::kMaxOutput> m_liquidLeftTmp{};
    std::array<float, fm_tuner::dsp::liquid::Resampler::kMaxOutput> m_liquidRightTmp{};
};

#endif
