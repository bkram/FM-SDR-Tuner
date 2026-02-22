#ifndef FM_DEMOD_H
#define FM_DEMOD_H

#include <stdint.h>
#include <stddef.h>
#include <array>
#include <vector>
#include "dsp/liquid_primitives.h"

class FMDemod {
public:
    enum class DemodMode {
        Fast = 0,
        Exact = 1
    };

    FMDemod(int inputRate, int outputRate);
    ~FMDemod();

    void process(const uint8_t* iq, float* audio, size_t numSamples);
    void processNoDownsample(const uint8_t* iq, float* audio, size_t numSamples);
    size_t processSplit(const uint8_t* iq, float* mpxOut, float* monoOut, size_t numSamples);
    size_t downsampleAudio(const float* demod, float* audio, size_t numSamples);
    void reset();

    void setDeemphasis(int tau_us);
    void setDeviation(double deviation);
    void setBandwidthMode(int mode);
    void setBandwidthHz(int bwHz);
    void setDemodMode(DemodMode mode);
    DemodMode getDemodMode() const { return m_demodMode; }
    bool isClipping() const { return m_clipping; }
    float getClippingRatio() const { return m_clippingRatio; }

private:
    void demodulate(const uint8_t* iq, float* audio, size_t len);

    int m_inputRate;
    int m_outputRate;

    DemodMode m_demodMode;
    float m_lastPhase;
    bool m_haveLastPhase;
    float m_prevI;
    float m_prevQ;
    bool m_havePrevIQ;
    double m_deviation;
    double m_invDeviation;

    float m_deemphAlpha;
    float m_deemphasisState;
    int m_bandwidthMode;

    std::vector<float> m_demodScratch;
    float m_iqPrevInI;
    float m_iqPrevInQ;
    float m_iqDcStateI;
    float m_iqDcStateQ;

    bool m_clipping;
    float m_clippingRatio;
    fm_tuner::dsp::liquid::FIRFilter m_liquidIqFilter;
    fm_tuner::dsp::liquid::Resampler m_liquidMonoResampler;
    std::array<float, fm_tuner::dsp::liquid::Resampler::kMaxOutput> m_liquidResampleTmp{};
};

#endif
