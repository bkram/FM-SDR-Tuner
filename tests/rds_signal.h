#ifndef FM_TUNER_TESTS_RDS_SIGNAL_H
#define FM_TUNER_TESTS_RDS_SIGNAL_H

// Synthetic RDS for tests: known groups encoded as 26-bit blocks (16 data
// bits + checkword + offset word, IEC 62106 annex B), and an MPX generator
// that carries them as differentially coded biphase BPSK at 1187.5 bit/s on
// 57 kHz, phase-locked to a 19 kHz pilot.

#include <cmath>
#include <cstdint>
#include <random>
#include <utility>
#include <vector>

namespace rds_signal {

struct Group {
  uint16_t a, b, c, d;
};

// Offset words A, B, C, D (IEC 62106 Table B.1).
constexpr uint16_t kOffsetA = 0x0FC;
constexpr uint16_t kOffsetB = 0x198;
constexpr uint16_t kOffsetC = 0x168;
constexpr uint16_t kOffsetD = 0x1B4;

inline uint32_t checkword(uint16_t data) {
  uint32_t reg = static_cast<uint32_t>(data) << 10;
  for (int i = 25; i >= 10; i--) {
    if (reg & (1u << i)) {
      reg ^= 0x5B9u << (i - 10);
    }
  }
  return reg & 0x3FFu;
}

inline uint32_t blockWord(uint16_t data, uint16_t offset) {
  return (static_cast<uint32_t>(data) << 10) | (checkword(data) ^ offset);
}

// `count` groups with random B/C/D. Block A (the PI code) is the group index
// plus `piBase`, so an error-free block A identifies the group it came from.
inline std::vector<Group> makeGroups(int count, uint16_t piBase,
                                     uint32_t seed = 42) {
  std::mt19937 rng(seed);
  std::vector<Group> groups;
  for (int i = 0; i < count; i++) {
    groups.push_back(Group{static_cast<uint16_t>(piBase + i),
                           static_cast<uint16_t>(rng() & 0xF7FFu),
                           static_cast<uint16_t>(rng()),
                           static_cast<uint16_t>(rng())});
  }
  return groups;
}

inline std::vector<int> groupsToBits(const std::vector<Group> &groups) {
  std::vector<int> bits;
  for (const Group &g : groups) {
    const uint32_t words[4] = {blockWord(g.a, kOffsetA), blockWord(g.b, kOffsetB),
                               blockWord(g.c, kOffsetC), blockWord(g.d, kOffsetD)};
    for (uint32_t w : words) {
      for (int bit = 25; bit >= 0; bit--) {
        bits.push_back(static_cast<int>((w >> bit) & 1u));
      }
    }
  }
  return bits;
}

// MPX samples carrying `bits`: pilot 9%, RDS at `rdsAmp`, optional noise.
class MpxGenerator {
public:
  MpxGenerator(double sampleRate, std::vector<int> bits, double rdsAmp = 0.04,
               double noiseSigma = 0.0)
      : m_fs(sampleRate), m_bits(std::move(bits)), m_rdsAmp(rdsAmp),
        m_noise(noiseSigma) {}

  float next() {
    const double tb = m_t * 1187.5;
    const size_t bitIndex = static_cast<size_t>(tb);
    const double frac = tb - static_cast<double>(bitIndex);
    if (bitIndex != m_bitIndex || m_t == 0.0) {
      m_bitIndex = bitIndex;
      const int data = bitIndex < m_bits.size() ? m_bits[bitIndex] : 0;
      m_encoded ^= data;
    }
    const double manchester = (frac < 0.5 ? 1.0 : -1.0) * (m_encoded ? 1.0 : -1.0);
    // Two one-pole low-passes at ~2.4 kHz round the biphase edges.
    const double a = 1.0 - std::exp(-2.0 * M_PI * 2400.0 / m_fs);
    m_lp1 += a * (manchester - m_lp1);
    m_lp2 += a * (m_lp1 - m_lp2);
    const double theta = 2.0 * M_PI * 19000.0 * m_t;
    double s = 0.09 * std::sin(theta) + m_rdsAmp * m_lp2 * std::sin(3.0 * theta);
    if (m_noise > 0.0) {
      s += m_noise * m_gauss(m_rng);
    }
    m_t += 1.0 / m_fs;
    return static_cast<float>(s);
  }

private:
  double m_fs;
  std::vector<int> m_bits;
  double m_rdsAmp;
  double m_noise;
  double m_t = 0.0;
  size_t m_bitIndex = 0;
  int m_encoded = 0;
  double m_lp1 = 0.0;
  double m_lp2 = 0.0;
  std::mt19937 m_rng{1234};
  std::normal_distribution<double> m_gauss{0.0, 1.0};
};

} // namespace rds_signal

#endif
