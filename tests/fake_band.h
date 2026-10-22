#pragma once

// A synthetic SDR front end for the scan-engine tests.
//
// The band holds ONE station on a known ABSOLUTE frequency plus noise. The
// station is only present in a capture when it actually falls inside the
// captured band, exactly like a real front end. Modelling it by absolute
// frequency (rather than by an offset from a hard-coded tune frequency)
// keeps the tests independent of the scan engine's internal capture
// geometry: a test states "there is a station on 96.2 MHz" and does not have
// to know where the engine decides to point the tuner.
//
// The station is a broad hump of tones, the way the spectrum of a real FM
// station looks: its carrier and its immediate sidebands occupy about 30 kHz
// and its skirt reaches out to +-100 kHz. A single tone would be captured in
// full by every measurement window that contains it, so the peak of the
// measured curve would sit at the edge of the window instead of at the
// station, and the reported frequency would be off by up to half a
// measurement window.
//
// The tones sit half a step off the carrier: a real FM station carries no
// unmodulated carrier line, and a tone exactly at the centre of a capture
// would be a DC line that the estimator's mean removal strips.

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
        m_currentHz(0), m_rng(12345) {
    // The hump tapers linearly from the carrier to its edges, so the band
    // power of a measurement window has a single maximum at the carrier.
    constexpr int kToneCount = 9;
    constexpr double kToneSpacingHz = 6000.0;
    constexpr double kToneSpanHz = 30000.0;
    double norm = 0.0;
    for (int j = 0; j < kToneCount; ++j) {
      const double off =
          ((static_cast<double>(j) - (kToneCount - 1) / 2.0) * kToneSpacingHz) +
          (kToneSpacingHz / 2.0);
      const double shape = 1.0 - std::abs(off) / kToneSpanHz;
      m_toneOffsets.push_back(off);
      m_toneShapes.push_back(shape);
      norm += shape * shape;
    }
    m_toneNorm = std::sqrt(norm);
    m_toneRe.assign(m_toneOffsets.size(), 1.0);
    m_toneIm.assign(m_toneOffsets.size(), 0.0);
    m_stepRe.assign(m_toneOffsets.size(), 1.0);
    m_stepIm.assign(m_toneOffsets.size(), 0.0);
  }

  void setTune(uint32_t hz) {
    m_currentHz = hz;
    // The tuner shifts the station down to the centre of the capture, so
    // every tone of the hump sits at its offset from the capture centre.
    const double fs = static_cast<double>(m_sampleRateHz);
    for (size_t j = 0; j < m_toneOffsets.size(); ++j) {
      double freq = static_cast<double>(m_carrierHz) + m_toneOffsets[j] -
                    static_cast<double>(hz);
      // Fold the frequency into the sampled band, the way the converter
      // does when the station sits outside of it.
      freq = std::fmod(freq + fs / 2.0, fs) - fs / 2.0;
      const double step = 2.0 * kPi * freq / fs;
      m_stepRe[j] = std::cos(step);
      m_stepIm[j] = std::sin(step);
      m_toneRe[j] = 1.0;
      m_toneIm[j] = 0.0;
    }
  }

  uint32_t currentHz() const { return m_currentHz; }

  size_t read(uint8_t *dst) {
    const int64_t offset = static_cast<int64_t>(m_carrierHz) -
                            static_cast<int64_t>(m_currentHz);
    const int64_t fs = static_cast<int64_t>(m_sampleRateHz);
    // The station is only in the capture when the whole hump fits into the
    // sampled band, the way the tuner's analogue filter rejects it
    // otherwise.
    const bool inBand = (m_carrierHz != 0) && (m_amplitude > 0.0f) &&
                        (std::llabs(offset) + kHumpSpanHz / 2 < fs / 2);
    std::uniform_int_distribution<int> dist(
        -static_cast<int>(m_noise * 127.5f),
        static_cast<int>(m_noise * 127.5f));
    for (size_t i = 0; i < m_samples; i++) {
      float iSample = dist(m_rng) / 127.5f;
      float qSample = dist(m_rng) / 127.5f;
      if (inBand) {
        double accRe = 0.0;
        double accIm = 0.0;
        for (size_t j = 0; j < m_toneOffsets.size(); ++j) {
          const double re = m_toneRe[j];
          const double im = m_toneIm[j];
          m_toneRe[j] = re * m_stepRe[j] - im * m_stepIm[j];
          m_toneIm[j] = re * m_stepIm[j] + im * m_stepRe[j];
          const double amp =
              (static_cast<double>(m_amplitude) * m_toneShapes[j]) / m_toneNorm;
          accRe += amp * m_toneRe[j];
          accIm += amp * m_toneIm[j];
        }
        iSample += static_cast<float>(accRe);
        qSample += static_cast<float>(accIm);
      }
      dst[i * 2] = static_cast<uint8_t>(
          std::clamp(std::lround(iSample * 127.5f + 127.5f), 0L, 255L));
      dst[i * 2 + 1] = static_cast<uint8_t>(
          std::clamp(std::lround(qSample * 127.5f + 127.5f), 0L, 255L));
    }
    return m_samples;
  }

private:
  static constexpr float kPi = 3.14159265358979323846f;
  static constexpr int64_t kHumpSpanHz = 27000;

  uint32_t m_sampleRateHz;
  uint32_t m_carrierHz;
  size_t m_samples;
  float m_amplitude;
  float m_noise;
  uint32_t m_currentHz;
  std::mt19937 m_rng;
  std::vector<double> m_toneOffsets;
  std::vector<double> m_toneShapes;
  double m_toneNorm = 1.0;
  std::vector<double> m_toneRe;
  std::vector<double> m_toneIm;
  std::vector<double> m_stepRe;
  std::vector<double> m_stepIm;
};
