#ifndef AUDIO_BACKEND_H
#define AUDIO_BACKEND_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class IAudioBackend {
public:
  virtual ~IAudioBackend() = default;

  virtual bool init(const std::string &deviceSelector, bool verboseLogging) = 0;
  virtual void shutdown() = 0;
  virtual bool write(const float *left, const float *right,
                     size_t numSamples) = 0;
  virtual void setVolumePercent(int volumePercent) = 0;
  virtual bool isRunning() const = 0;
  virtual bool listDevices() = 0;
};

class AudioFileRecorder {
public:
  AudioFileRecorder();
  ~AudioFileRecorder();

  bool init(const std::string &filename);
  void shutdown();
  bool write(const float *left, const float *right, size_t numSamples);
  bool isRecording() const { return m_isRecording; }

private:
  void writeHeader();
  bool m_isRecording = false;
  FILE *m_fileHandle = nullptr;
  uint32_t m_dataSize = 0;
  int m_sampleRate = 48000;
  int m_channels = 2;
  int m_bitsPerSample = 16;
};

IAudioBackend *createAudioBackend();
void destroyAudioBackend(IAudioBackend *backend);

#endif
