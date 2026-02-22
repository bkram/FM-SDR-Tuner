#ifndef STEREO_DECODER_H
#define STEREO_DECODER_H

#include <stddef.h>
#include <stdint.h>
#include <cmath>
#include <vector>
#include "dsp/liquid_primitives.h"

class StereoDecoder {
public:
    enum class BlendMode {
        Soft = 0,
        Normal = 1,
        Aggressive = 2
    };

    StereoDecoder(int inputRate, int outputRate);
    ~StereoDecoder();

    size_t processAudio(const float* mono, float* left, float* right, size_t numSamples);
    void reset();
    void setForceStereo(bool force);
    void setForceMono(bool force);
    void setBlendMode(BlendMode mode) { m_blendMode = mode; }
    int getPilotLevelTenthsKHz() const { return m_pilotLevelTenthsKHz; }

    bool isStereo() const { return m_stereoDetected; }

private:
    int m_inputRate;
    bool m_stereoDetected;
    bool m_forceStereo;
    bool m_forceMono;
    BlendMode m_blendMode;
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
    int m_pilotCount;
    int m_pilotLossCount;

    std::vector<float> m_delayLine;
    size_t m_delayPos;
    int m_delaySamples;
    fm_tuner::dsp::liquid::FIRFilter m_liquidPilotBandFilter;
    fm_tuner::dsp::liquid::NCO m_liquidPilotPll;
    fm_tuner::dsp::liquid::FIRFilter m_liquidLeftAudioFilter;
    fm_tuner::dsp::liquid::FIRFilter m_liquidRightAudioFilter;
};

#endif
