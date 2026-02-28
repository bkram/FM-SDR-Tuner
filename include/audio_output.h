#ifndef AUDIO_OUTPUT_H
#define AUDIO_OUTPUT_H

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <stddef.h>
#include <stdint.h>
#include <string>
#include <thread>
#include <vector>

#if defined(FM_TUNER_HAS_PORTAUDIO)
#include <portaudio.h>
#elif defined(__APPLE__) && defined(FM_TUNER_HAS_COREAUDIO)
#include <AudioUnit/AudioUnit.h>
#elif defined(_WIN32) && defined(FM_TUNER_HAS_WINMM)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>
#elif defined(__linux__)
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

  static constexpr int kMaxVolumePercent = 100;
  static constexpr float kDefaultVolumeScale = 0.85f;
  static constexpr float kInt16Max = 32767.0f;
  static constexpr size_t kCircularBufferSize = 65536;
  static constexpr float kVolumeEpsilon = 1e-6f;

  AudioOutput();
  ~AudioOutput();

  bool init(bool enableSpeaker, const std::string &wavFile,
            const std::string &deviceSelector = "", bool verboseLogging = true);
  void shutdown();
  void setVolumePercent(int volumePercent);
  static bool listDevices();

  bool write(const float *left, const float *right, size_t numSamples);
  void clearRealtimeQueue();
  bool isRunning() const { return m_running; }

private:
  bool initWAV(const std::string &filename);
  void writeWAVHeader();
  bool writeWAVData(const float *left, const float *right, size_t numSamples);
  void closeWAV();
  void runOutputThread();
  void runAlsaOutputThread();
  static bool listAlsaDevices();
#if defined(__APPLE__) && defined(FM_TUNER_HAS_COREAUDIO)
  static bool listCoreAudioDevices();
  static OSStatus
  coreAudioRenderCallback(void *inRefCon,
                          AudioUnitRenderActionFlags *ioActionFlags,
                          const AudioTimeStamp *inTimeStamp, UInt32 inBusNumber,
                          UInt32 inNumberFrames, AudioBufferList *ioData);
#endif
#if defined(_WIN32) && defined(FM_TUNER_HAS_WINMM)
  static bool listWinMMDevices();
  void runWinMMOutputThread();
#endif
  bool initAlsa(const std::string &deviceName);
  void shutdownAlsa();

#if defined(FM_TUNER_HAS_PORTAUDIO)
  static int paCallback(const void *inputBuffer, void *outputBuffer,
                        unsigned long framesPerBuffer,
                        const PaStreamInfo *timeInfo, PaStreamFlags statusFlags,
                        void *userData);
#endif

  bool m_enableSpeaker;
  std::string m_wavFile;
  FILE *m_wavHandle;
  std::atomic<bool> m_running;
  uint32_t m_wavDataSize;

  std::vector<float> m_circularBuffer;
  std::atomic<int> m_writeIndex;
  std::atomic<int> m_readIndex;
  bool m_verboseLogging;
  std::atomic<int> m_requestedVolumePercent;
  float m_currentVolumeScale;

#if defined(FM_TUNER_HAS_PORTAUDIO)
  PaStream *m_paStream;
  bool m_portAudioInitialized;
  std::thread m_outputThread;
  std::atomic<bool> m_outputThreadRunning;
  std::mutex m_outputMutex;
  std::condition_variable m_outputCv;
  std::vector<float> m_outputQueue;
  size_t m_outputReadIndex;
#endif

#if defined(__APPLE__) && defined(FM_TUNER_HAS_COREAUDIO)
  AudioUnit m_audioUnit;
  std::mutex m_outputMutex;
  std::condition_variable m_outputCv;
  std::vector<float> m_outputQueue;
  size_t m_outputReadIndex;
  double m_coreAudioOutputRate;
  double m_coreAudioSourcePerDest;
  double m_coreAudioResamplePhase;
#endif

#if defined(_WIN32) && defined(FM_TUNER_HAS_WINMM)
  HWAVEOUT m_waveOut;
  std::thread m_winmmThread;
  std::atomic<bool> m_winmmThreadRunning;
  std::mutex m_outputMutex;
  std::condition_variable m_outputCv;
  std::vector<float> m_outputQueue;
  size_t m_outputReadIndex;
#endif

#if defined(__linux__) && defined(FM_TUNER_HAS_ALSA)
  snd_pcm_t *m_alsaPcm;
  std::thread m_alsaOutputThread;
  std::atomic<bool> m_alsaThreadRunning;
  std::mutex m_alsaMutex;
  std::condition_variable m_alsaCv;
  std::vector<float> m_alsaBuffer;
  size_t m_alsaReadIndex;
#endif
};

#endif
