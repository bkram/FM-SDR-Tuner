#ifndef WAV_WRITER_H
#define WAV_WRITER_H

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class WavWriter {
public:
  static constexpr uint16_t BITS_PER_SAMPLE = 16;
  static constexpr float kInt16Max = 32767.0f;

  WavWriter();
  ~WavWriter();

  bool init(const std::string &filename, uint32_t sampleRate, uint16_t channels,
            bool verboseLogging, const char *label);
  void shutdown();
  bool isOpen() const { return m_handle != nullptr; }

  bool enqueueInterleavedFloat(const float *samples, size_t frameCount);
  bool enqueueMonoFloat(const float *samples, size_t sampleCount);

private:
  bool writeHeader();
  bool writeData(const int16_t *samples, size_t sampleCount);
  void runWriterThread();

  FILE *m_handle;
  std::atomic<bool> m_threadRunning;
  std::atomic<bool> m_fatalError;
  uint32_t m_dataSize;
  uint32_t m_sampleRate;
  uint16_t m_channels;
  bool m_verboseLogging;
  std::string m_label;

  std::mutex m_mutex;
  std::condition_variable m_cv;
  std::vector<int16_t> m_ring;
  std::vector<int16_t> m_encodeScratch;
  size_t m_readPos;
  size_t m_writePos;
  size_t m_size;
  std::thread m_thread;
};

#endif
