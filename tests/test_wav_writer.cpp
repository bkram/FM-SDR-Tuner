#include "catch_compat.h"

#include <atomic>
#include <condition_variable>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "dsp/liquid_primitives.h"
#include "wav_limits.h"

// The writer's internals (the payload counter, the fatal-error flag and the
// private write path) are the only way to reach the 4 GiB limit without
// writing 4 GiB, so the test opens the class up.
#define private public
#include "wav_writer.h"
#undef private

namespace {

uint32_t readLe32(const std::vector<uint8_t> &b, size_t off) {
  return static_cast<uint32_t>(b[off]) |
         (static_cast<uint32_t>(b[off + 1]) << 8) |
         (static_cast<uint32_t>(b[off + 2]) << 16) |
         (static_cast<uint32_t>(b[off + 3]) << 24);
}

uint16_t readLe16(const std::vector<uint8_t> &b, size_t off) {
  return static_cast<uint16_t>(static_cast<uint16_t>(b[off]) |
                                (static_cast<uint16_t>(b[off + 1]) << 8));
}

} // namespace

TEST_CASE("WavWriter writes valid mono WAV header and payload", "[wav_writer]") {
  const std::string path = "test_mpx_output.wav";
  std::remove(path.c_str());

  WavWriter writer;
  REQUIRE(writer.init(path, 256000, 1, false, "MPX WAV"));
  const float samples[8] = {0.0f, 0.25f, -0.25f, 0.5f, -0.5f, 1.2f, -1.2f, 0.1f};
  REQUIRE(writer.enqueueMonoFloat(samples, 8));
  writer.shutdown();

  std::ifstream f(path, std::ios::binary);
  REQUIRE(f.good());
  const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)),
                                   std::istreambuf_iterator<char>());
  REQUIRE(bytes.size() >= 44);
  REQUIRE(std::string(reinterpret_cast<const char *>(bytes.data()), 4) ==
          "RIFF");
  REQUIRE(std::string(reinterpret_cast<const char *>(bytes.data() + 8), 4) ==
          "WAVE");
  REQUIRE(readLe16(bytes, 20) == 1);
  REQUIRE(readLe16(bytes, 22) == 1);
  REQUIRE(readLe32(bytes, 24) == 256000);
  REQUIRE(readLe16(bytes, 34) == WavWriter::BITS_PER_SAMPLE);
  REQUIRE(readLe32(bytes, 40) == static_cast<uint32_t>(8 * sizeof(int16_t)));

  std::remove(path.c_str());
}

TEST_CASE("WavWriter resamples mono MPX from input rate to header rate",
          "[wav_writer][mpx_resample]") {
  const std::string path = "test_mpx_resampled.wav";
  std::remove(path.c_str());

  constexpr uint32_t kInputRate = 256000;  // native MPX
  constexpr uint32_t kTargetRate = 192000; // analysis-friendly
  constexpr size_t kInputSamples = 4 * kInputRate; // 4 s of audio

  WavWriter writer;
  REQUIRE(writer.init(path, kTargetRate, 1, false, "MPX WAV", kInputRate));

  // Generate a 19 kHz pilot-shaped tone — well below both rates' Nyquist so it
  // should survive resampling intact.
  constexpr float kTwoPi = 6.2831853071795864769f;
  std::vector<float> input(kInputSamples, 0.0f);
  for (size_t i = 0; i < kInputSamples; i++) {
    const float t = static_cast<float>(i) / static_cast<float>(kInputRate);
    input[i] = 0.30f * std::sin(kTwoPi * 19000.0f * t);
  }
  // Feed in production-sized chunks so the resampler's per-call output count
  // gets exercised the way the live pipeline drives it.
  constexpr size_t kChunk = 8192;
  for (size_t off = 0; off < kInputSamples; off += kChunk) {
    const size_t n = std::min(kChunk, kInputSamples - off);
    REQUIRE(writer.enqueueMonoFloat(input.data() + off, n));
  }
  writer.shutdown();

  std::ifstream f(path, std::ios::binary);
  REQUIRE(f.good());
  const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)),
                                   std::istreambuf_iterator<char>());
  REQUIRE(bytes.size() >= 44);
  REQUIRE(readLe32(bytes, 24) == kTargetRate); // header reflects target rate
  const uint32_t dataSize = readLe32(bytes, 40);
  // 4 s of mono int16 at 192 kHz = 1.536 MB ± a small resampler edge effect.
  const uint32_t expectedBytes =
      4U * kTargetRate * static_cast<uint32_t>(sizeof(int16_t));
  REQUIRE(dataSize > expectedBytes * 95 / 100);
  REQUIRE(dataSize < expectedBytes * 105 / 100);

  // Decode payload, verify the 19 kHz tone is still there at roughly the
  // expected amplitude (no rate-induced spectral collapse).
  const size_t numOut = dataSize / sizeof(int16_t);
  std::vector<float> output(numOut);
  for (size_t i = 0; i < numOut; i++) {
    int16_t s = static_cast<int16_t>(bytes[44 + i * 2] |
                                     (bytes[44 + i * 2 + 1] << 8));
    output[i] = static_cast<float>(s) / 32768.0f;
  }
  // Skip the first 100 ms to avoid the resampler's startup transient.
  const size_t skip = kTargetRate / 10;
  REQUIRE(numOut > skip + kTargetRate);
  double acc = 0.0;
  for (size_t i = skip; i < numOut; i++) {
    acc += static_cast<double>(output[i]) * output[i];
  }
  const float rms =
      static_cast<float>(std::sqrt(acc / static_cast<double>(numOut - skip)));
  // 0.30 amplitude sine → RMS ≈ 0.212. Allow ±15% for filter ringing.
  REQUIRE(rms > 0.18f);
  REQUIRE(rms < 0.25f);

  std::remove(path.c_str());
}

TEST_CASE("WavWriter pass-through path (no resample) is unaffected",
          "[wav_writer]") {
  const std::string path = "test_mpx_passthrough.wav";
  std::remove(path.c_str());

  WavWriter writer;
  // resampleFromHz == sampleRate → must NOT engage the resampler.
  REQUIRE(writer.init(path, 256000, 1, false, "MPX WAV", 256000));
  const float samples[4] = {0.1f, -0.1f, 0.5f, -0.5f};
  REQUIRE(writer.enqueueMonoFloat(samples, 4));
  writer.shutdown();

  std::ifstream f(path, std::ios::binary);
  REQUIRE(f.good());
  const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)),
                                   std::istreambuf_iterator<char>());
  REQUIRE(readLe32(bytes, 24) == 256000);
  // Exact sample count — pass-through path doesn't add or drop samples.
  REQUIRE(readLe32(bytes, 40) == 4U * sizeof(int16_t));

  std::remove(path.c_str());
}

TEST_CASE("WavWriter flushes the queued samples when it shuts down",
          "[wav_writer]") {
  const std::string path = "test_wav_flush.wav";
  std::remove(path.c_str());

  // One second of stereo audio: more than the writer thread drains in one
  // chunk, so at shutdown there is still a backlog in the ring.
  constexpr size_t kFrames = 48000;
  std::vector<float> interleaved(kFrames * 2, 0.0f);
  for (size_t i = 0; i < kFrames; i++) {
    interleaved[i * 2] = 0.5f;
    interleaved[i * 2 + 1] = -0.5f;
  }

  WavWriter writer;
  REQUIRE(writer.init(path, 48000, 2, false, "Audio WAV"));
  REQUIRE(writer.enqueueInterleavedFloat(interleaved.data(), kFrames));
  // shutdown() has to drain the ring before it closes the file.
  writer.shutdown();

  std::ifstream f(path, std::ios::binary);
  REQUIRE(f.good());
  const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)),
                                   std::istreambuf_iterator<char>());
  const uint32_t dataSize = readLe32(bytes, 40);
  REQUIRE(dataSize == static_cast<uint32_t>(kFrames * 2 * sizeof(int16_t)));
  REQUIRE(bytes.size() == static_cast<size_t>(44 + dataSize));

  // Every queued sample has to be in the file, in order.
  for (size_t i = 0; i < kFrames; i++) {
    const int16_t left = static_cast<int16_t>(bytes[44 + i * 4] |
                                              (bytes[44 + i * 4 + 1] << 8));
    const int16_t right = static_cast<int16_t>(bytes[44 + i * 4 + 2] |
                                               (bytes[44 + i * 4 + 3] << 8));
    REQUIRE(left == 16383);
    REQUIRE(right == -16383);
  }

  std::remove(path.c_str());
}

TEST_CASE("the WAV payload limit leaves room for the container overhead",
          "[wav_writer]") {
  // The RIFF size field has to cover the payload *and* the 36 bytes of
  // container overhead, so the payload stops 36 bytes short of 4 GiB.
  REQUIRE(kWavMaxDataBytes ==
          static_cast<size_t>(UINT32_MAX) - kWavFileOverheadBytes);
  REQUIRE(kWavMaxDataBytes + kWavFileOverheadBytes ==
          static_cast<size_t>(UINT32_MAX));

  // The limit is inclusive: the last byte that fits is still allowed.
  REQUIRE(wavDataFits(0, kWavMaxDataBytes));
  REQUIRE_FALSE(wavDataFits(0, kWavMaxDataBytes + 1));
  REQUIRE(wavDataFits(kWavMaxDataBytes, 0));
  REQUIRE_FALSE(wavDataFits(kWavMaxDataBytes, 1));
  REQUIRE_FALSE(wavDataFits(kWavMaxDataBytes + 1, 0));
}

TEST_CASE("WavWriter refuses a write that would cross the 4 GiB limit",
          "[wav_writer]") {
  const std::string path = "test_wav_limit.wav";
  std::remove(path.c_str());

  WavWriter writer;
  REQUIRE(writer.init(path, 256000, 1, false, "MPX WAV"));

  const int16_t samples[5] = {1, 2, 3, 4, 5};
  // Pretend the file already holds everything but the last 10 bytes: five
  // samples still fit, a sixth does not.
  writer.m_dataSize = static_cast<uint32_t>(kWavMaxDataBytes - 10);
  REQUIRE(writer.writeData(samples, 5));
  REQUIRE(writer.m_dataSize == kWavMaxDataBytes);
  REQUIRE_FALSE(writer.m_fatalError.load());

  // One sample too many: the write is refused and the capture stops.
  REQUIRE_FALSE(writer.writeData(samples, 1));
  REQUIRE(writer.m_fatalError.load());

  // The latched error is what refuses the write, not the size: with room
  // again, a later write is still refused.
  writer.m_dataSize = 0;
  REQUIRE_FALSE(writer.writeData(samples, 1));

  writer.shutdown();
  std::remove(path.c_str());
}
