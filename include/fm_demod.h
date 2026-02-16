#ifndef FM_DEMOD_H
#define FM_DEMOD_H

#include <stdint.h>
#include <stddef.h>

class FMDemod {
public:
    FMDemod(int inputRate, int outputRate);
    ~FMDemod();

    void process(const int8_t* iq, float* audio, size_t numSamples);
    void processNoDownsample(const int8_t* iq, float* audio, size_t numSamples);
    void reset();

    void setDeemphasis(int tau_us);
    void setOversample(int os);
    void setDeviation(double deviation);

private:
    void lowPass(float* buffer, size_t len, float cutoff);
    void downsample(const float* input, float* output, size_t inputLen, size_t* outputLen);
    void demodulate(const float* iq, float* audio, size_t len);
    void deemphasize(float* audio, size_t len);

    int m_inputRate;
    int m_outputRate;
    int m_downsampleFactor;
    int m_oversample;

    float* m_firBuffer;
    size_t m_firLen;

    float m_lastPhase;
    double m_deviation;
    double m_invDeviation;

    int m_deemphA;
    float m_deemphasisState;
};

#endif
