#pragma once

// A synthetic SDR front end for the scan-engine tests.
//
// The band holds ONE carrier on a known ABSOLUTE frequency plus noise. The
// carrier is only present in a capture when it actually falls inside the
// captured band, exactly like a real front end. Modelling the carrier by
// absolute frequency (rather than by an offset from a hard-coded tune
// frequency) keeps the tests independent of the scan engine's internal
// capture geometry: a test states "there is a station on 96.2 MHz" and does
// not have to know where the engine decides to point the tuner.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

class FakeBand {
public:
  FakeBand(uint32_t sampleRateHz, uint32_t carrierHz, size_t samples,
           float amplitude, float noise)
      : m_sampleRateHz(sampleRateHz), m_carrierHz(carrierHz),
        m_samples(samples), m_amplitude(amplitude), m_noise(noise),
        m_currentHz(0), m_rng(12345) {}

  void setTune(uint32_t hz) { m_currentHz = hz; }

  uint32_t currentHz() const { return m_currentHz; }

  size_t read(uint8_t *dst) {
    const int64_t offset = static_cast<int64_t>(m_carrierHz) -
                            static_cast<int64_t>(m_currentHz);
    const int64_t fs = static_cast<int64_t>(m_sampleRateHz);
    const bool inBand = (offset > -fs / 2) && (offset < fs / 2);
    std::uniform_int_distribution<int> dist(
        -static_cast<int>(m_noise * 127.5f),
        static_cast<int>(m_noise * 127.5f));
    constexpr float kPi = 3.14159265358979323846f;
    for (size_t i = 0; i < m_samples; i++) {
      float iSample = dist(m_rng) / 127.5f;
      float qSample = dist(m_rng) / 127.5f;
      if (inBand) {
        const float phase = 2.0f * kPi * static_cast<float>(offset) *
                             (static_cast<float>(i) /
                              static_cast<float>(m_sampleRateHz));
        iSample += std::cos(phase) * m_amplitude;
        qSample += std::sin(phase) * m_amplitude;
      }
      dst[i * 2] = static_cast<uint8_t>(
          std::clamp(std::lround(iSample * 127.5f + 127.5f), 0L, 255L));
      dst[i * 2 + 1] = static_cast<uint8_t>(
          std::clamp(std::lround(qSample * 127.5f + 127.5f), 0L, 255L));
    }
    return m_samples;
  }

private:
  uint32_t m_sampleRateHz;
  uint32_t m_carrierHz;
  size_t m_samples;
  float m_amplitude;
  float m_noise;
  uint32_t m_currentHz;
  std::mt19937 m_rng;
};
