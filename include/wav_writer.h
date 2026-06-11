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

#include "dsp/liquid_primitives.h"

class WavWriter {
public:
  static constexpr uint16_t BITS_PER_SAMPLE = 16;
  static constexpr float kInt16Max = 32767.0f;

  WavWriter();
  ~WavWriter();

  // If resampleFromHz > 0 and differs from sampleRate, samples passed to
  // enqueueMonoFloat() are assumed to arrive at resampleFromHz and are
  // resampled to sampleRate before being written. The WAV header always
  // reflects sampleRate. Only the mono path supports resampling; the
  // interleaved-float path is for already-at-rate audio.
  bool init(const std::string &filename, uint32_t sampleRate, uint16_t channels,
            bool verboseLogging, const char *label,
            uint32_t resampleFromHz = 0);
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
  // False when the output is a pipe/FIFO/char device (not seekable): the
  // header is written once, streaming, and never patched on close.
  bool m_seekable = true;
  std::string m_label;

  std::mutex m_mutex;
  std::condition_variable m_cv;
  std::vector<int16_t> m_ring;
  std::vector<int16_t> m_encodeScratch;
  size_t m_readPos;
  size_t m_writePos;
  size_t m_size;
  std::thread m_thread;

  // Optional input-side resampler for the mono path (e.g. MPX at 256 kHz →
  // 192 kHz). Inactive when m_resampleEnabled is false.
  bool m_resampleEnabled;
  fm_tuner::dsp::liquid::Resampler m_resampler;
  std::array<float, fm_tuner::dsp::liquid::Resampler::kMaxOutput>
      m_resampleTmp{};
  std::vector<float> m_resampleAccum;
};

#endif
