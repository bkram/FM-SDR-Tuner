#include "catch_compat.h"

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
