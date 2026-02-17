#ifndef FM_DEMOD_H
#define FM_DEMOD_H

#include <stdint.h>
#include <stddef.h>
#include <vector>

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
    size_t downsampleAudio(const float* demod, float* audio, size_t numSamples);
    void reset();

    void setDeemphasis(int tau_us);
    void setDeviation(double deviation);
    void setBandwidthMode(int mode);
    void setBandwidthHz(int bwHz);
    void setDemodMode(DemodMode mode);
    DemodMode getDemodMode() const { return m_demodMode; }

private:
    void demodulate(const uint8_t* iq, float* audio, size_t len);
    void initAudioFilter();
    void rebuildAudioFilter(double cutoffHz);

    int m_inputRate;
    int m_outputRate;
    int m_downsampleFactor;

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

    std::vector<float> m_audioTaps;
    std::vector<float> m_audioTapsRev;
    std::vector<float> m_audioHistoryLinear;
    std::vector<float> m_demodScratch;
    size_t m_audioHistPos;
    int m_decimPhase;
};

#endif
