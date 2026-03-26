#include "wav_writer.h"

#include <algorithm>
#include <chrono>
#include <iostream>

namespace {

constexpr size_t kDefaultQueueSeconds = 2;
constexpr size_t kWriterChunkSamples = 8192;

} // namespace

WavWriter::WavWriter()
    : m_handle(nullptr), m_threadRunning(false), m_dataSize(0),
      m_sampleRate(0), m_channels(0), m_verboseLogging(true), m_readPos(0),
      m_writePos(0), m_size(0) {}

WavWriter::~WavWriter() { shutdown(); }

bool WavWriter::init(const std::string &filename, uint32_t sampleRate,
                     uint16_t channels, bool verboseLogging,
                     const char *label) {
  shutdown();
  if (filename.empty() || sampleRate == 0 || channels == 0) {
    return false;
  }

  m_handle = std::fopen(filename.c_str(), "wb");
  if (!m_handle) {
    return false;
  }

  m_sampleRate = sampleRate;
  m_channels = channels;
  m_verboseLogging = verboseLogging;
  m_label = (label != nullptr) ? label : "WAV";
  m_dataSize = 0;
  m_readPos = 0;
  m_writePos = 0;
  m_size = 0;
  m_ring.assign(static_cast<size_t>(sampleRate) * channels * kDefaultQueueSeconds,
                0);
  m_encodeScratch.clear();

  writeHeader();
  m_threadRunning = true;
  m_thread = std::thread(&WavWriter::runWriterThread, this);
  return true;
}

void WavWriter::shutdown() {
  if (!m_handle) {
    return;
  }
  m_threadRunning = false;
  m_cv.notify_all();
  if (m_thread.joinable()) {
    m_thread.join();
  }
  writeHeader();
  std::fclose(m_handle);
  m_handle = nullptr;
}

void WavWriter::writeHeader() {
  if (!m_handle) {
    return;
  }

  const uint32_t byteRate = m_sampleRate * m_channels * BITS_PER_SAMPLE / 8;
  const uint16_t blockAlign = m_channels * BITS_PER_SAMPLE / 8;
  const uint32_t dataSize = m_dataSize;

  std::fseek(m_handle, 0, SEEK_SET);
  std::fwrite("RIFF", 1, 4, m_handle);
  const uint32_t fileSize = 36 + dataSize;
  std::fwrite(&fileSize, 4, 1, m_handle);
  std::fwrite("WAVE", 1, 4, m_handle);

  std::fwrite("fmt ", 1, 4, m_handle);
  const uint32_t fmtSize = 16;
  std::fwrite(&fmtSize, 4, 1, m_handle);
  const uint16_t audioFormat = 1;
  std::fwrite(&audioFormat, 2, 1, m_handle);
  std::fwrite(&m_channels, 2, 1, m_handle);
  std::fwrite(&m_sampleRate, 4, 1, m_handle);
  std::fwrite(&byteRate, 4, 1, m_handle);
  std::fwrite(&blockAlign, 2, 1, m_handle);
  std::fwrite(&BITS_PER_SAMPLE, 2, 1, m_handle);

  std::fwrite("data", 1, 4, m_handle);
  std::fwrite(&dataSize, 4, 1, m_handle);
}

bool WavWriter::writeData(const int16_t *samples, size_t sampleCount) {
  if (!m_handle || !samples || sampleCount == 0) {
    return false;
  }
  const size_t written =
      std::fwrite(samples, sizeof(int16_t), sampleCount, m_handle);
  m_dataSize += static_cast<uint32_t>(written * sizeof(int16_t));
  return written == sampleCount;
}

bool WavWriter::enqueueInterleavedFloat(const float *samples,
                                        size_t frameCount) {
  if (!m_handle || !samples || frameCount == 0 || m_channels == 0 ||
      m_ring.empty()) {
    return false;
  }

  const size_t sampleCount = frameCount * m_channels;
  if (m_encodeScratch.size() < sampleCount) {
    m_encodeScratch.resize(sampleCount);
  }
  for (size_t i = 0; i < sampleCount; i++) {
    const float clamped = std::clamp(samples[i], -1.0f, 1.0f);
    m_encodeScratch[i] = static_cast<int16_t>(clamped * kInt16Max);
  }

  std::lock_guard<std::mutex> lock(m_mutex);
  const size_t capacity = m_ring.size();
  size_t start = 0;
  if (sampleCount >= capacity) {
    start = sampleCount - capacity;
  }
  const size_t kept = sampleCount - start;
  if (m_size + kept > capacity) {
    size_t drop = m_size + kept - capacity;
    drop = std::min(drop, m_size);
    m_readPos = (m_readPos + drop) % capacity;
    m_size -= drop;
    if (m_verboseLogging) {
      static std::atomic<uint32_t> overflowCount{0};
      const uint32_t count = ++overflowCount;
      if (count <= 5 || (count % 50) == 0) {
        std::cerr << "[AUDIO] " << m_label << " queue overflow (" << count
                  << ")\n";
      }
    }
  }

  for (size_t i = start; i < sampleCount; i++) {
    m_ring[m_writePos] = m_encodeScratch[i];
    m_writePos = (m_writePos + 1) % capacity;
  }
  m_size += kept;
  m_cv.notify_one();
  return true;
}

bool WavWriter::enqueueMonoFloat(const float *samples, size_t sampleCount) {
  if (m_channels != 1) {
    return false;
  }
  return enqueueInterleavedFloat(samples, sampleCount);
}

void WavWriter::runWriterThread() {
  std::vector<int16_t> localBuffer(kWriterChunkSamples, 0);
  while (true) {
    size_t copied = 0;
    {
      std::unique_lock<std::mutex> lock(m_mutex);
      m_cv.wait_for(lock, std::chrono::milliseconds(50), [&]() {
        return !m_threadRunning.load() || m_size > 0;
      });
      if (!m_threadRunning.load() && m_size == 0) {
        break;
      }
      if (m_size == 0) {
        continue;
      }
      copied = std::min(localBuffer.size(), m_size);
      for (size_t i = 0; i < copied; i++) {
        localBuffer[i] = m_ring[m_readPos];
        m_readPos = (m_readPos + 1) % m_ring.size();
      }
      m_size -= copied;
    }
    (void)writeData(localBuffer.data(), copied);
  }
}
