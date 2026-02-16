#ifndef AF_POST_PROCESSOR_H
#define AF_POST_PROCESSOR_H

#include <stddef.h>

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
    double m_resampleStep;
    double m_resamplePos;
    bool m_deemphasisEnabled;
    float m_deemphAlpha;
    float m_deemphStateLeft;
    float m_deemphStateRight;
};

#endif
