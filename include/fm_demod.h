#ifndef FM_DEMOD_H
#define FM_DEMOD_H

#include <stdint.h>
#include <stddef.h>
#include <vector>

class FMDemod {
public:
    FMDemod(int inputRate, int outputRate);
    ~FMDemod();

    void process(const uint8_t* iq, float* audio, size_t numSamples);
    void processNoDownsample(const uint8_t* iq, float* audio, size_t numSamples);
    size_t downsampleAudio(const float* demod, float* audio, size_t numSamples);
    void reset();

    void setDeemphasis(int tau_us);
    void setDeviation(double deviation);
    void setBandwidthMode(int mode);
    void setBandwidthHz(int bwHz);

private:
    void demodulate(const uint8_t* iq, float* audio, size_t len);
    void initAudioFilter();
    void rebuildAudioFilter(double cutoffHz);

    int m_inputRate;
    int m_outputRate;
    int m_downsampleFactor;

    float m_lastPhase;
    double m_deviation;
    double m_invDeviation;

    float m_deemphAlpha;
    float m_deemphasisState;
    int m_bandwidthMode;

    std::vector<float> m_audioTaps;
    std::vector<float> m_audioHistory;
    std::vector<float> m_demodScratch;
    size_t m_audioHistPos;
    int m_decimPhase;
};

#endif
