#ifndef AUDIO_OUTPUT_H
#define AUDIO_OUTPUT_H

#include <stdint.h>
#include <stddef.h>
#include <string>
#include <atomic>
#include <vector>

#ifdef __APPLE__
#include <portaudio.h>
#endif

class AudioOutput {
public:
    static constexpr int SAMPLE_RATE = 32000;
    static constexpr int CHANNELS = 2;
    static constexpr int BITS_PER_SAMPLE = 16;
    static constexpr int FRAMES_PER_BUFFER = 1024;

    AudioOutput();
    ~AudioOutput();

    bool init(bool enableSpeaker, const std::string& wavFile);
    void shutdown();

    bool write(const float* left, const float* right, size_t numSamples);
    bool isRunning() const { return m_running; }

private:
    bool initWAV(const std::string& filename);
    void writeWAVHeader();
    bool writeWAVData(const float* left, const float* right, size_t numSamples);
    void closeWAV();

#ifdef __APPLE__
    static int paCallback(const void* inputBuffer, void* outputBuffer,
                          unsigned long framesPerBuffer,
                          const PaStreamInfo* timeInfo,
                          PaStreamFlags statusFlags,
                          void* userData);
#endif

    bool m_enableSpeaker;
    std::string m_wavFile;
    FILE* m_wavHandle;
    std::atomic<bool> m_running;
    uint32_t m_wavDataSize;

    std::vector<float> m_circularBuffer;
    std::atomic<int> m_writeIndex;
    std::atomic<int> m_readIndex;

#ifdef __APPLE__
    PaStream* m_paStream;
#endif
};

#endif
