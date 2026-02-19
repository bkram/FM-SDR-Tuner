#ifndef AUDIO_OUTPUT_H
#define AUDIO_OUTPUT_H

#include <stdint.h>
#include <stddef.h>
#include <string>
#include <atomic>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>

#if defined(__APPLE__)
#include <portaudio.h>
#elif defined(__linux__)
#include <portaudio.h>
#if defined(FM_TUNER_HAS_ALSA)
#include <alsa/asoundlib.h>
#endif
#endif

class AudioOutput {
public:
    static constexpr int SAMPLE_RATE = 32000;
    static constexpr int CHANNELS = 2;
    static constexpr int BITS_PER_SAMPLE = 16;
    static constexpr int FRAMES_PER_BUFFER = 4096;

    AudioOutput();
    ~AudioOutput();

    bool init(bool enableSpeaker, const std::string& wavFile, const std::string& deviceSelector = "", bool verboseLogging = true);
    void shutdown();
    void setVolumePercent(int volumePercent);
    void setUnderflowFadeMs(int fadeMs);
    static bool listDevices();

    bool write(const float* left, const float* right, size_t numSamples);
    bool isRunning() const { return m_running; }

private:
    bool initWAV(const std::string& filename);
    void writeWAVHeader();
    bool writeWAVData(const float* left, const float* right, size_t numSamples);
    void closeWAV();
    void runOutputThread();
    void runAlsaOutputThread();
    static bool listAlsaDevices();
    bool initAlsa(const std::string& deviceName);
    void shutdownAlsa();

#if defined(__APPLE__) || defined(__linux__)
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
    bool m_verboseLogging;
    std::atomic<int> m_requestedVolumePercent;
    std::atomic<int> m_underflowFadeMs;
    float m_currentVolumeScale;

#if defined(__APPLE__) || defined(__linux__)
    PaStream* m_paStream;
    bool m_portAudioInitialized;
    std::thread m_outputThread;
    std::atomic<bool> m_outputThreadRunning;
    std::mutex m_outputMutex;
    std::condition_variable m_outputCv;
    std::vector<float> m_outputQueue;
    size_t m_outputReadIndex;
#endif

#if defined(__linux__) && defined(FM_TUNER_HAS_ALSA)
    snd_pcm_t* m_alsaPcm;
    std::thread m_alsaOutputThread;
    std::atomic<bool> m_alsaThreadRunning;
    std::mutex m_alsaMutex;
    std::condition_variable m_alsaCv;
    std::vector<float> m_alsaBuffer;
    size_t m_alsaReadIndex;
#endif
};

#endif
