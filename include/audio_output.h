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

#if defined(__APPLE__) && defined(FM_TUNER_HAS_COREAUDIO)
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
  static constexpr int SAMPLE_RATE = 48000;
  static constexpr int CHANNELS = 2;
  static constexpr int BITS_PER_SAMPLE = 16;
  static constexpr int FRAMES_PER_BUFFER = 4096;

  static constexpr int kMaxVolumePercent = 100;
  static constexpr float kDefaultVolumeScale = 0.85f;
  static constexpr float kInt16Max = 32767.0f;
  static constexpr size_t kCircularBufferSize = 65536;
  // ~2 s of speaker-ring headroom. A gain change / retune briefly stalls the
  // USB IQ read, then a backlog burst arrives and is demodulated faster than
  // real time; a small ring dropped those samples (audible stutter on every
  // auto-gain step). The output thread keeps up in steady state, so the ring
  // runs near-empty and this larger capacity only absorbs bursts — it adds no
  // steady-state latency.
  static constexpr size_t kSpeakerQueueSamples =
      static_cast<size_t>(SAMPLE_RATE) * CHANNELS * 2;
  static constexpr size_t kWavQueueSamples =
      static_cast<size_t>(SAMPLE_RATE) * CHANNELS * 2;
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
  bool writeWAVHeader();
  bool writeWAVData(const int16_t *samples, size_t sampleCount);
  void closeWAV();
  void runWavWriterThread();
  void runAlsaOutputThread();
  void clearSpeakerQueueLocked();
  void logSpeakerOverflow(const char *backendLabel, uint32_t count) const;
  void pushSpeakerSamples(const float *left, const float *right, size_t numSamples,
                         const char *backendLabel);
  size_t popSpeakerSamplesLocked(float *dest, size_t maxSamples);
  bool enqueueWavSamples(const float *left, const float *right, size_t numSamples);
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

  bool m_enableSpeaker;
  std::string m_wavFile;
  FILE *m_wavHandle;
  std::atomic<bool> m_running;
  std::atomic<bool> m_wavThreadRunning;
  std::atomic<bool> m_wavFatalError;
  uint32_t m_wavDataSize;

  bool m_verboseLogging;
  std::atomic<int> m_requestedVolumePercent;
  float m_currentVolumeScale;
  std::vector<float> m_scaledLeftScratch;
  std::vector<float> m_scaledRightScratch;
  std::vector<float> m_speakerScratch;
  std::vector<int16_t> m_wavEncodeScratch;
  std::mutex m_speakerMutex;
  std::condition_variable m_speakerCv;
  std::vector<float> m_speakerRing;
  size_t m_speakerReadPos;
  size_t m_speakerWritePos;
  size_t m_speakerSize;
  std::mutex m_wavMutex;
  std::condition_variable m_wavCv;
  std::vector<int16_t> m_wavRing;
  size_t m_wavReadPos;
  size_t m_wavWritePos;
  size_t m_wavSize;
  std::thread m_wavThread;

#if defined(__APPLE__) && defined(FM_TUNER_HAS_COREAUDIO)
  AudioUnit m_audioUnit;
#endif

#if defined(_WIN32) && defined(FM_TUNER_HAS_WINMM)
  HWAVEOUT m_waveOut;
  // Auto-reset event signaled by the audio driver on each buffer completion
  // (CALLBACK_EVENT). The output thread blocks on this instead of polling, so
  // it is not throttled by Windows' coarse default timer resolution.
  HANDLE m_winmmEvent;
  std::thread m_winmmThread;
  std::atomic<bool> m_winmmThreadRunning;
#endif

#if defined(__linux__) && defined(FM_TUNER_HAS_ALSA)
  snd_pcm_t *m_alsaPcm;
  std::thread m_alsaOutputThread;
  std::atomic<bool> m_alsaThreadRunning;
#endif
};

#endif
