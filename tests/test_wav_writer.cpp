#include "catch_compat.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "wav_writer.h"

namespace {

uint32_t readLe32(const std::vector<uint8_t> &b, size_t off) {
  return static_cast<uint32_t>(b[off]) |
         (static_cast<uint32_t>(b[off + 1]) << 8) |
         (static_cast<uint32_t>(b[off + 2]) << 16) |
         (static_cast<uint32_t>(b[off + 3]) << 24);
}

uint16_t readLe16(const std::vector<uint8_t> &b, size_t off) {
  return static_cast<uint16_t>(b[off]) |
         (static_cast<uint16_t>(b[off + 1]) << 8);
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
