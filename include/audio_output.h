#ifndef AUDIO_OUTPUT_H
#define AUDIO_OUTPUT_H

#include <stdint.h>
#include <stddef.h>
#include <string>
#include <atomic>
#include <vector>

#ifdef __APPLE__
#include <AudioToolbox/AudioQueue.h>
#endif

class AudioOutput {
public:
    static constexpr int SAMPLE_RATE = 32000;
    static constexpr int CHANNELS = 2;
    static constexpr int BITS_PER_SAMPLE = 16;
    static constexpr int QUEUE_BUFFER_FRAMES = 4096;
    static constexpr int NUM_QUEUE_BUFFERS = 3;
    static constexpr int CIRCULAR_BUFFER_FRAMES = 30000;

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
    static void audioQueueCallback(void* inUserData, AudioQueueRef inAQ, AudioQueueBufferRef inBuffer);
    bool initAudioQueue();
    void stopAudioQueue();
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
    AudioQueueRef m_audioQueue;
    AudioQueueBufferRef m_buffers[NUM_QUEUE_BUFFERS];
#endif
};

#endif
