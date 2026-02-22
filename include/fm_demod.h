#ifndef FM_DEMOD_H
#define FM_DEMOD_H

#include <stdint.h>
#include <stddef.h>
#include <array>
#include <vector>
#include "dsp/liquid_primitives.h"

class FMDemod {
public:
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
    void setW0BandwidthHz(int bwHz);
    bool isClipping() const { return m_clipping; }
    float getClippingRatio() const { return m_clippingRatio; }

private:
    void demodulate(const uint8_t* iq, float* audio, size_t len);

    int m_inputRate;
    int m_outputRate;

    double m_deviation;

    bool m_deemphasisEnabled;
    int m_bandwidthMode;
    int m_w0BandwidthHz;

    std::vector<float> m_demodScratch;

    bool m_clipping;
    float m_clippingRatio;
    fm_tuner::dsp::liquid::FIRFilter m_liquidIqFilter;
    fm_tuner::dsp::liquid::FreqDemod m_liquidFreqDemod;
    fm_tuner::dsp::liquid::IIRFilterReal m_liquidIqDcBlockI;
    fm_tuner::dsp::liquid::IIRFilterReal m_liquidIqDcBlockQ;
    fm_tuner::dsp::liquid::IIRFilterReal m_liquidMonoDeemphasis;
    fm_tuner::dsp::liquid::IIRFilterReal m_liquidMonoDcBlock;
    fm_tuner::dsp::liquid::Resampler m_liquidMonoResampler;
    std::array<float, fm_tuner::dsp::liquid::Resampler::kMaxOutput> m_liquidResampleTmp{};
};

#endif
