#ifndef STEREO_DECODER_H
#define STEREO_DECODER_H

#include <stddef.h>
#include <stdint.h>
#include <cmath>
#include <cstring>

class StereoDecoder {
public:
    StereoDecoder(int sampleRate);
    ~StereoDecoder();

    void process(const float* mono, float* left, float* right, size_t numSamples);
    void reset();
    void setForceStereo(bool force);
    void setForceMono(bool force);

    bool isStereo() const { return m_stereoDetected; }
    void setStereo(bool stereo) { m_forceStereo = stereo; }

private:
    bool detectPilotPLL(const float* audio, size_t len);
    void decodeStereo(const float* mono, float* left, float* right, size_t len);

    int m_sampleRate;
    bool m_stereoDetected;
    bool m_forceStereo;
    bool m_forceMono;

    float m_pilotPhase;
    float m_pilotFreq;
    float m_pilotAlpha;
    float m_pilotBeta;
    int m_pilotCount;

    float m_audioDelay[512];
    int m_delayIndex;
    int m_delayLen;
};

#endif
