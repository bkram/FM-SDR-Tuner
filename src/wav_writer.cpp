#include "wav_writer.h"

#include <algorithm>
#include <chrono>
#include <iostream>

#include <sys/stat.h>

namespace {

constexpr size_t kDefaultQueueSeconds = 2;
constexpr size_t kWriterChunkSamples = 8192;

} // namespace

WavWriter::WavWriter()
    : m_handle(nullptr), m_threadRunning(false), m_fatalError(false),
      m_dataSize(0), m_sampleRate(0), m_channels(0), m_verboseLogging(true),
      m_readPos(0), m_writePos(0), m_size(0), m_resampleEnabled(false) {}

WavWriter::~WavWriter() { shutdown(); }

bool WavWriter::init(const std::string &filename, uint32_t sampleRate,
                     uint16_t channels, bool verboseLogging, const char *label,
                     uint32_t resampleFromHz) {
  shutdown();
  if (filename.empty() || sampleRate == 0 || channels == 0) {
    return false;
  }

  m_handle = std::fopen(filename.c_str(), "wb");
  if (!m_handle) {
    return false;
  }

  // A pipe/FIFO/char device (e.g. --mpx-wav <fifo> or /dev/stdout) is not
  // seekable: write the WAV header once, streaming, and never patch sizes on
  // close. Only a regular file can have its header rewritten.
  struct stat st {};
  m_seekable =
      (fstat(fileno(m_handle), &st) == 0) && S_ISREG(st.st_mode);

  m_sampleRate = sampleRate;
  m_channels = channels;
  m_verboseLogging = verboseLogging;
  m_label = (label != nullptr) ? label : "WAV";
  m_dataSize = 0;
  m_fatalError.store(false);
  m_readPos = 0;
  m_writePos = 0;
  m_size = 0;
  m_ring.assign(static_cast<size_t>(sampleRate) * channels * kDefaultQueueSeconds,
                0);
  m_encodeScratch.clear();
  m_resampleEnabled = false;
  m_resampleAccum.clear();

  if (resampleFromHz != 0 && resampleFromHz != sampleRate && channels == 1) {
    const float ratio =
        static_cast<float>(sampleRate) / static_cast<float>(resampleFromHz);
    try {
      m_resampler.init(ratio);
      m_resampleEnabled = true;
      if (m_verboseLogging) {
        std::cout << "[AUDIO] " << m_label << " resample "
                  << resampleFromHz << " Hz -> " << sampleRate << " Hz (ratio "
                  << ratio << ")\n";
      }
    } catch (const std::exception &ex) {
      std::cerr << "[AUDIO] " << m_label
                << " resampler init failed: " << ex.what() << "\n";
      std::fclose(m_handle);
      m_handle = nullptr;
      return false;
    }
  }

  if (!writeHeader()) {
    std::fclose(m_handle);
    m_handle = nullptr;
    return false;
  }
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
  // Patch the header sizes only on a seekable (regular) file. On a stream the
  // header was already written once at the front; rewriting now would inject
  // 44 bytes into the middle of the audio data.
  if (m_seekable && !writeHeader() && !m_fatalError.load() && m_verboseLogging) {
    std::cerr << "[AUDIO] " << m_label << " final header write failed\n";
  }
  std::fclose(m_handle);
  m_handle = nullptr;
}

bool WavWriter::writeHeader() {
  if (!m_handle) {
    return false;
  }

  const uint32_t byteRate = m_sampleRate * m_channels * BITS_PER_SAMPLE / 8;
  const uint16_t blockAlign = m_channels * BITS_PER_SAMPLE / 8;
  // Streaming (non-seekable) output cannot have its sizes patched on close, so
  // advertise the maximum -- downstream consumers read to EOF.
  const uint32_t dataSize = m_seekable ? m_dataSize : 0xFFFFFFFFu;
  const uint32_t fileSize = m_seekable ? (36u + m_dataSize) : 0xFFFFFFFFu;
  const uint32_t fmtSize = 16;
  const uint16_t audioFormat = 1;
  const uint16_t bitsPerSample = BITS_PER_SAMPLE;

  // Only seek a regular file; on a pipe/FIFO the header is written in place
  // at the start of the stream and never rewritten.
  if (m_seekable && std::fseek(m_handle, 0, SEEK_SET) != 0) {
    if (m_verboseLogging) {
      std::cerr << "[AUDIO] " << m_label << " failed to seek WAV header\n";
    }
    return false;
  }

  const bool ok =
      std::fwrite("RIFF", 1, 4, m_handle) == 4 &&
      std::fwrite(&fileSize, 4, 1, m_handle) == 1 &&
      std::fwrite("WAVE", 1, 4, m_handle) == 4 &&
      std::fwrite("fmt ", 1, 4, m_handle) == 4 &&
      std::fwrite(&fmtSize, 4, 1, m_handle) == 1 &&
      std::fwrite(&audioFormat, 2, 1, m_handle) == 1 &&
      std::fwrite(&m_channels, 2, 1, m_handle) == 1 &&
      std::fwrite(&m_sampleRate, 4, 1, m_handle) == 1 &&
      std::fwrite(&byteRate, 4, 1, m_handle) == 1 &&
      std::fwrite(&blockAlign, 2, 1, m_handle) == 1 &&
      std::fwrite(&bitsPerSample, 2, 1, m_handle) == 1 &&
      std::fwrite("data", 1, 4, m_handle) == 4 &&
      std::fwrite(&dataSize, 4, 1, m_handle) == 1;
  if (!ok && m_verboseLogging) {
    std::cerr << "[AUDIO] " << m_label << " failed to write WAV header\n";
  }
  return ok;
}

bool WavWriter::writeData(const int16_t *samples, size_t sampleCount) {
  if (!m_handle || !samples || sampleCount == 0) {
    return false;
  }
  if (m_fatalError.load()) {
    return false;
  }
  const size_t maxBytes =
      static_cast<size_t>(UINT32_MAX) - 36u - m_dataSize;
  const size_t wantBytes = sampleCount * sizeof(int16_t);
  if (wantBytes > maxBytes) {
    if (m_verboseLogging) {
      std::cerr << "[AUDIO] " << m_label
                << " reached 4 GiB WAV limit, stopping capture\n";
    }
    m_fatalError.store(true);
    return false;
  }
  const size_t written =
      std::fwrite(samples, sizeof(int16_t), sampleCount, m_handle);
  m_dataSize += static_cast<uint32_t>(written * sizeof(int16_t));
  if (written != sampleCount) {
    if (m_verboseLogging) {
      std::cerr << "[AUDIO] " << m_label << " short WAV write (" << written
                << "/" << sampleCount << ")\n";
    }
    m_fatalError.store(true);
    return false;
  }
  return true;
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
  if (!m_resampleEnabled) {
    return enqueueInterleavedFloat(samples, sampleCount);
  }
  // Push each input sample through the resampler and collect outputs into the
  // accumulator. Once the accumulator has data we flush it through the
  // existing path so the rest of the pipeline is unchanged.
  if (m_resampleAccum.capacity() == 0) {
    m_resampleAccum.reserve(sampleCount); // typical 1 in -> ~1 out
  }
  m_resampleAccum.clear();
  for (size_t i = 0; i < sampleCount; i++) {
    const std::uint32_t produced =
        m_resampler.execute(samples[i], m_resampleTmp);
    for (std::uint32_t p = 0; p < produced; p++) {
      m_resampleAccum.push_back(m_resampleTmp[p]);
    }
  }
  if (m_resampleAccum.empty()) {
    return true; // nothing to write yet — common when downsampling
  }
  return enqueueInterleavedFloat(m_resampleAccum.data(), m_resampleAccum.size());
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
    if (!writeData(localBuffer.data(), copied)) {
      break;
    }
  }
}
