#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#define private public
#include "audio_output.h"
#undef private

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

TEST_CASE("AudioOutput WAV path writes valid header and PCM payload",
          "[audio_output]") {
  const std::string path = "test_audio_output.wav";
  std::remove(path.c_str());

  AudioOutput out;
  REQUIRE(out.init(false, path, "", false));
  REQUIRE(out.isRunning());

  constexpr size_t n = 8;
  const std::array<float, n> left = {0.0f, 0.25f, -0.25f, 0.5f, -0.5f, 1.2f,
                                     -1.2f, 0.1f};
  const std::array<float, n> right = {0.0f, -0.25f, 0.25f, -0.5f, 0.5f, -1.2f,
                                      1.2f, -0.1f};
  REQUIRE(out.write(left.data(), right.data(), n));
  out.shutdown();
  REQUIRE_FALSE(out.isRunning());

  std::ifstream f(path, std::ios::binary);
  REQUIRE(f.good());
  const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)),
                                   std::istreambuf_iterator<char>());
  REQUIRE(bytes.size() >= 44);

  REQUIRE(std::string(reinterpret_cast<const char *>(bytes.data()), 4) ==
          "RIFF");
  REQUIRE(std::string(reinterpret_cast<const char *>(bytes.data() + 8), 4) ==
          "WAVE");
  REQUIRE(std::string(reinterpret_cast<const char *>(bytes.data() + 12), 4) ==
          "fmt ");
  REQUIRE(std::string(reinterpret_cast<const char *>(bytes.data() + 36), 4) ==
          "data");

  REQUIRE(readLe16(bytes, 20) == 1); // PCM
  REQUIRE(readLe16(bytes, 22) == AudioOutput::CHANNELS);
  REQUIRE(readLe32(bytes, 24) == AudioOutput::SAMPLE_RATE);
  REQUIRE(readLe16(bytes, 34) == AudioOutput::BITS_PER_SAMPLE);

  const uint32_t dataSize = readLe32(bytes, 40);
  REQUIRE(dataSize == static_cast<uint32_t>(n * AudioOutput::CHANNELS *
                                            sizeof(int16_t)));
  REQUIRE(bytes.size() == static_cast<size_t>(44 + dataSize));

  std::remove(path.c_str());
}

TEST_CASE("AudioOutput write fails when not running and volume clamps",
          "[audio_output]") {
  AudioOutput out;
  const float l[2] = {0.0f, 0.0f};
  const float r[2] = {0.0f, 0.0f};
  REQUIRE_FALSE(out.write(l, r, 2));

  out.setVolumePercent(-100);
  REQUIRE(out.m_requestedVolumePercent.load() == 0);
  out.setVolumePercent(250);
  REQUIRE(out.m_requestedVolumePercent.load() == AudioOutput::kMaxVolumePercent);
}
