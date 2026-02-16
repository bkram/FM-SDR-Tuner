#ifndef STEREO_DECODER_H
#define STEREO_DECODER_H

#include <stddef.h>
#include <stdint.h>
#include <cmath>
#include <cstring>
#include <vector>

class StereoDecoder {
public:
    StereoDecoder(int inputRate, int outputRate);
    ~StereoDecoder();

    size_t processAudio(const float* mono, float* left, float* right, size_t numSamples);
    void reset();
    void setForceStereo(bool force);
    void setForceMono(bool force);
    int getPilotLevelTenthsKHz() const { return m_pilotLevelTenthsKHz; }

    bool isStereo() const { return m_stereoDetected; }

private:
    float filterSample(float input, const std::vector<float>& taps, std::vector<float>& history, size_t& pos);
    std::vector<float> designLowPass(double cutoffHz, double transitionHz) const;
    std::vector<float> designBandPass(double lowHz, double highHz, double transitionHz) const;
    float windowNuttall(int n, int count) const;

    int m_inputRate;
    bool m_stereoDetected;
    bool m_forceStereo;
    bool m_forceMono;
    float m_pilotMagnitude;
    float m_pilotBandMagnitude;
    float m_mpxMagnitude;
    float m_stereoBlend;
    int m_pilotLevelTenthsKHz;
    float m_pilotI;
    float m_pilotQ;

    float m_pllPhase;
    float m_pllFreq;
    float m_pllMinFreq;
    float m_pllMaxFreq;
    float m_pllAlpha;
    float m_pllBeta;
    int m_pilotCount;
    int m_pilotLossCount;

    std::vector<float> m_pilotTaps;
    std::vector<float> m_pilotTapsRev;
    std::vector<float> m_pilotHistory;
    size_t m_pilotHistPos;

    std::vector<float> m_audioTaps;
    std::vector<float> m_audioTapsRev;
    std::vector<float> m_leftHistory;
    std::vector<float> m_rightHistory;
    size_t m_leftHistPos;
    size_t m_rightHistPos;
    bool m_useNeon;
    bool m_useSse2;
    bool m_useAvx2;

    std::vector<float> m_delayLine;
    size_t m_delayPos;
    int m_delaySamples;
};

#endif
