#include "stereo_decoder.h"
#include <cmath>
#include <cstring>
#include <algorithm>

StereoDecoder::StereoDecoder(int sampleRate)
    : m_sampleRate(sampleRate)
    , m_stereoDetected(false)
    , m_forceStereo(false)
    , m_pilotPhase(0)
    , m_pilotFreq(2.0f * 3.14159f * 19000.0f / sampleRate)
    , m_pilotAlpha(0.01f)
    , m_pilotBeta(0.0001f)
    , m_pilotCount(0)
    , m_delayIndex(0)
    , m_delayLen(64) {
    memset(m_audioDelay, 0, sizeof(m_audioDelay));
}

StereoDecoder::~StereoDecoder() {
}

void StereoDecoder::reset() {
    m_pilotPhase = 0;
    m_pilotFreq = 2.0f * 3.14159f * 19000.0f / m_sampleRate;
    m_pilotCount = 0;
    m_stereoDetected = false;
    m_delayIndex = 0;
    memset(m_audioDelay, 0, sizeof(m_audioDelay));
}

void StereoDecoder::setForceStereo(bool force) {
    m_forceStereo = force;
}

bool StereoDecoder::detectPilotPLL(const float* audio, size_t len) {
    if (len < 512) {
        return false;
    }

    float errorSum = 0;
    int count = std::min(len, (size_t)2048);

    for (int i = 0; i < count; i++) {
        float pilotI = cosf(m_pilotPhase);
        float pilotQ = sinf(m_pilotPhase);

        errorSum += audio[i] * pilotQ;

        m_pilotPhase += m_pilotFreq + m_pilotBeta * errorSum;

        while (m_pilotPhase > 2.0f * 3.14159f) {
            m_pilotPhase -= 2.0f * 3.14159f;
        }
        while (m_pilotPhase < 0) {
            m_pilotPhase += 2.0f * 3.14159f;
        }
    }

    float pilotMagnitude = errorSum / count;
    float threshold = 0.001f;

    return pilotMagnitude > threshold;
}

void StereoDecoder::decodeStereo(const float* mono, float* left, float* right, size_t len) {
    float subcarrierFreq = 2.0f * m_pilotFreq;
    float phase = m_pilotPhase;

    for (size_t i = 0; i < len; i++) {
        float subI = cosf(phase);
        float subQ = sinf(phase);

        float mpx = mono[i];
        float lr = mpx * 2.0f * subI;

        left[i] = mpx + lr;
        right[i] = mpx - lr;

        phase += subcarrierFreq;
        while (phase > 2.0f * 3.14159f) {
            phase -= 2.0f * 3.14159f;
        }
    }

    m_pilotPhase = phase;
}

void StereoDecoder::process(const float* mono, float* left, float* right, size_t numSamples) {
    if (!m_forceStereo && !m_stereoDetected) {
        if (detectPilotPLL(mono, numSamples)) {
            m_pilotCount++;
            if (m_pilotCount > 5) {
                m_stereoDetected = true;
            }
        } else {
            m_pilotCount = 0;
        }
    }

    if (m_forceStereo || m_stereoDetected) {
        decodeStereo(mono, left, right, numSamples);
    } else {
        for (size_t i = 0; i < numSamples; i++) {
            left[i] = mono[i];
            right[i] = mono[i];
        }
    }
}
