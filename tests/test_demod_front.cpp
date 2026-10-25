#include "catch_compat.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <deque>
#include <random>
#include <vector>

#include "config.h"
#include "dsp/multipath_eq.h"
#include "dsp_pipeline.h"
#include "fm_demod.h"

// Front-end and demodulator regressions: the mono (stereo = false) audio
// path, the IQ DC blocker, bandwidth changes, the fade-mute timing at high IQ
// rates, the multipath equaliser's step size and the L1-normalised channel
// FIR's power reading. Each case reproduces a defect measured on synthetic
// signals and fails on the code before its fix.

namespace {

constexpr double kPi = 3.14159265358979323846;

// Broadcast FM generator. The multiplex is built the way a stereo encoder
// does it: 90% programme (L+R plus L-R on a 38 kHz subcarrier phase-locked to
// the pilot), a 10% pilot at 19 kHz and an optional RDS-band tone at 57 kHz.
// 1.0 of multiplex is 75 kHz deviation. Output is RTL-style uint8 IQ.
struct FmSource {
  explicit FmSource(double rate) : fs(rate) {}

  double fs;
  double amp = 0.5;
  double offsetHz = 0.0;
  double toneHz = 1000.0;
  double left = 0.5;
  double right = 0.0;
  bool pilot = true;
  bool stereo = true;
  double rds = 0.0;
  double noise = 0.0;
  // A static front-end DC offset, added to every IQ sample.
  std::complex<double> dc{0.0, 0.0};

  double t = 0.0;
  double phase = 0.0;
  std::mt19937 rng{7};
  std::normal_distribution<double> gauss{0.0, 1.0};

  double mpx() const {
    const double tone = std::sin(2.0 * kPi * toneHz * t);
    const double l = left * tone;
    const double r = right * tone;
    double m = 0.9 * 0.5 * (l + r);
    if (stereo) {
      m += 0.9 * 0.5 * (l - r) * std::sin(2.0 * kPi * 38000.0 * t);
    }
    if (pilot) {
      m += 0.1 * std::sin(2.0 * kPi * 19000.0 * t);
    }
    if (rds > 0.0) {
      m += rds * std::sin(2.0 * kPi * 57000.0 * t);
    }
    return m;
  }

  std::complex<double> next() {
    phase += 2.0 * kPi * (75000.0 * mpx() + offsetHz) / fs;
    if (phase > kPi) {
      phase -= 2.0 * kPi;
    }
    t += 1.0 / fs;
    return std::polar(amp, phase) + dc +
           std::complex<double>(noise * gauss(rng), noise * gauss(rng));
  }

  static uint8_t quantize(double v) {
    return static_cast<uint8_t>(
        std::clamp(std::round(127.5 + 127.5 * v), 0.0, 255.0));
  }

  void fill(std::vector<uint8_t> &iq, size_t samples) {
    iq.resize(samples * 2);
    for (size_t i = 0; i < samples; i++) {
      const std::complex<double> c = next();
      iq[2 * i] = quantize(c.real());
      iq[2 * i + 1] = quantize(c.imag());
    }
  }
};

// Amplitude of a tone, in dB re a full-scale sine (amplitude 1 -> 0 dB).
double toneDb(const std::vector<float> &x, double hz, double fs) {
  const double w = 2.0 * kPi * hz / fs;
  const double c = 2.0 * std::cos(w);
  double s1 = 0.0;
  double s2 = 0.0;
  for (float v : x) {
    const double s0 = v + c * s1 - s2;
    s2 = s1;
    s1 = s0;
  }
  const double p = s1 * s1 + s2 * s2 - c * s1 * s2;
  const double a = 2.0 * std::sqrt(std::max(p, 0.0)) /
                   static_cast<double>(x.size());
  return 20.0 * std::log10(a + 1e-12);
}

// Tone power over everything else (noise and distortion), in dB.
double toneToRestDb(const std::vector<float> &x, double hz, double fs) {
  double mean = 0.0;
  for (float v : x) {
    mean += v;
  }
  mean /= static_cast<double>(x.size());
  double total = 0.0;
  for (float v : x) {
    total += (v - mean) * (v - mean);
  }
  total /= static_cast<double>(x.size());
  const double a = std::pow(10.0, toneDb(x, hz, fs) / 20.0);
  const double tone = a * a / 2.0;
  return 10.0 * std::log10(tone / std::max(total - tone, 1e-15));
}

double analogDeemphasisDb(double hz, double tauSec) {
  const double wt = 2.0 * kPi * hz * tauSec;
  return -10.0 * std::log10(1.0 + wt * wt);
}

} // namespace

TEST_CASE("Mono audio path rejects the pilot, subcarrier and RDS",
          "[dsp][demod][mono]") {
  // stereo = false sends the raw multiplex through FMDemod's own resampler.
  // Without a band limit, the L-R sidebands (37/39 kHz) and the RDS band
  // (57 kHz) fold to 9-11 kHz, and the 19 kHz pilot passes untouched.
  Config::ProcessingSection processing;
  processing.stereo = false;
  DspPipeline dsp(256000, 48000, processing, false, 8192, 1);
  FmSource src(256000.0);
  src.rds = 0.04;
  // Off centre, so the DC blocker's own behaviour stays out of this check.
  src.offsetHz = 1000.0;
  // A tone that does not divide the sample rate: with 1 kHz the 8-bit
  // quantisation error repeats every cycle and lands exactly on the harmonics
  // at 9 and 11 kHz, which would mask the aliases this test looks for.
  src.toneHz = 1001.3;

  std::vector<uint8_t> iq;
  std::vector<float> left;
  DspPipeline::Result out;
  for (int block = 0; block < 60; block++) {
    src.fill(iq, 8192);
    dsp.process(iq.data(), 8192, nullptr, out);
    if (block >= 20) {
      left.insert(left.end(), out.left, out.left + out.outSamples);
    }
  }
  REQUIRE(left.size() > 48000);

  const double programme = toneDb(left, src.toneHz, 48000.0);
  INFO("programme " << programme << " dB");
  // The L-R sidebands at 38 kHz +/- f fold to 10 kHz -/+ f, the RDS band at
  // 57 kHz folds to 9 kHz, and the pilot sits at 19 kHz.
  for (double hz : {10000.0 - src.toneHz, 10000.0 + src.toneHz, 9000.0,
                    19000.0}) {
    const double rel = toneDb(left, hz, 48000.0) - programme;
    INFO(hz << " Hz at " << rel << " dB re programme");
    CHECK(rel <= -60.0);
  }
}

TEST_CASE("Mono audio path de-emphasis follows the analog curve",
          "[dsp][demod][mono]") {
  for (int tauUs : {50, 75}) {
    std::vector<double> levels;
    const std::vector<double> tones = {1000.0, 3000.0, 5000.0, 10000.0,
                                       15000.0};
    for (double hz : tones) {
      FMDemod demod(256000, 48000);
      demod.setDeemphasis(tauUs);
      FmSource src(256000.0);
      src.pilot = false;
      src.stereo = false;
      src.toneHz = hz;
      src.left = 0.1;
      src.right = 0.1;
      src.offsetHz = 1000.0;
      std::vector<uint8_t> iq;
      std::vector<float> audio(8192);
      std::vector<float> collected;
      for (int block = 0; block < 20; block++) {
        src.fill(iq, 8192);
        const size_t produced =
            demod.processSplit(iq.data(), nullptr, audio.data(), 8192);
        if (block >= 5) {
          collected.insert(collected.end(), audio.begin(),
                           audio.begin() + static_cast<long>(produced));
        }
      }
      levels.push_back(toneDb(collected, hz, 48000.0));
    }
    const double tau = tauUs * 1e-6;
    for (size_t i = 1; i < tones.size(); i++) {
      const double measured = levels[i] - levels[0];
      const double expected =
          analogDeemphasisDb(tones[i], tau) - analogDeemphasisDb(1000.0, tau);
      INFO("tau " << tauUs << " us, " << tones[i] << " Hz: measured "
                  << measured << " dB, analog " << expected << " dB");
      CHECK(std::abs(measured - expected) <= 0.25);
    }
  }
}

TEST_CASE("IQ DC blocker leaves a carrier a few hertz off centre intact",
          "[dsp][demod][dc]") {
  // A station tuned within ~20 Hz of the centre used to be notched by the
  // 20 Hz DC blocker: an unmodulated carrier became full-scale discriminator
  // noise, and a modulated one was distorted.
  auto run = [](double offsetHz, double toneLevel) {
    FMDemod demod(256000, 48000);
    FmSource src(256000.0);
    src.pilot = false;
    src.stereo = false;
    src.offsetHz = offsetHz;
    src.left = toneLevel;
    src.right = toneLevel;
    // The unmodulated runs get ~31 dB carrier-to-noise: a notched carrier
    // drops into the noise, which the discriminator turns into hiss. (Without
    // noise the discriminator ignores the carrier's amplitude.) The modulated
    // runs stay clean so the distortion check is not masked by hiss.
    src.noise = (toneLevel == 0.0) ? 0.01 : 0.0;
    std::vector<uint8_t> iq;
    std::vector<float> demodOut(8192);
    std::vector<float> collected;
    // 1.6 s settle, then 0.5 s measured.
    for (int block = 0; block < 66; block++) {
      src.fill(iq, 8192);
      demod.processNoDownsample(iq.data(), demodOut.data(), 8192);
      if (block >= 50) {
        collected.insert(collected.end(), demodOut.begin(), demodOut.end());
      }
    }
    return collected;
  };

  const auto refTone = run(1000.0, 0.1);
  const double refLevel = toneDb(refTone, 1000.0, 256000.0);
  const double refClean = toneToRestDb(refTone, 1000.0, 256000.0);

  auto residualRms = [](const std::vector<float> &x) {
    double mean = 0.0;
    for (float v : x) {
      mean += v;
    }
    mean /= static_cast<double>(x.size());
    double acc = 0.0;
    for (float v : x) {
      acc += (v - mean) * (v - mean);
    }
    return std::sqrt(acc / static_cast<double>(x.size()));
  };
  const double refSilence = residualRms(run(1000.0, 0.0));

  for (double offset : {2.0, 5.0, 20.0}) {
    const auto tone = run(offset, 0.1);
    const double level = toneDb(tone, 1000.0, 256000.0);
    const double clean = toneToRestDb(tone, 1000.0, 256000.0);
    INFO("offset " << offset << " Hz: tone " << level << " dB (ref "
                   << refLevel << "), tone/rest " << clean << " dB (ref "
                   << refClean << ")");
    CHECK(std::abs(level - refLevel) <= 0.3);
    CHECK(clean >= refClean - 3.0);

    const double silence = residualRms(run(offset, 0.0));
    INFO("unmodulated carrier residual " << silence << " (ref " << refSilence
                                         << ")");
    CHECK(silence <= 2.0 * refSilence);
  }
}

TEST_CASE("IQ DC tracker still removes a static front-end offset",
          "[dsp][demod][dc]") {
  // The slow tracker must still take out the RTL's static DC offset, which
  // sits on top of the carrier and distorts the discriminator. It starts at
  // the old fast corner after a reset, so the offset is gone well inside the
  // retune mute.
  auto run = [](std::complex<double> dc) {
    FMDemod demod(256000, 48000);
    FmSource src(256000.0);
    src.pilot = false;
    src.stereo = false;
    src.offsetHz = 1000.0;
    src.amp = 0.2;
    src.left = 0.1;
    src.right = 0.1;
    src.dc = dc;
    std::vector<uint8_t> iq;
    std::vector<float> demodOut(8192);
    std::vector<float> collected;
    for (int block = 0; block < 40; block++) {
      src.fill(iq, 8192);
      demod.processNoDownsample(iq.data(), demodOut.data(), 8192);
      if (block >= 24) {
        collected.insert(collected.end(), demodOut.begin(), demodOut.end());
      }
    }
    return toneToRestDb(collected, 1000.0, 256000.0);
  };
  const double clean = run({0.0, 0.0});
  const double withOffset = run({0.06, -0.04});
  INFO("tone/rest " << clean << " dB clean, " << withOffset
                    << " dB with a static offset 11 dB below the carrier");
  CHECK(withOffset >= clean - 1.0);
}

TEST_CASE("Bandwidth change keeps stereo and the programme level",
          "[dsp][demod][bandwidth]") {
  // A W command (or adaptive bandwidth) used to reset the whole demodulator
  // and the stereo decoder: stereo dropped for ~200 ms and the level dipped.
  Config::ProcessingSection processing;
  DspPipeline dsp(256000, 48000, processing, false, 8192, 1);
  FmSource src(256000.0);

  std::vector<uint8_t> iq;
  DspPipeline::Result out;
  auto blockRms = [&]() {
    double acc = 0.0;
    for (size_t i = 0; i < out.outSamples; i++) {
      acc += static_cast<double>(out.left[i]) * out.left[i];
    }
    return std::sqrt(acc / static_cast<double>(std::max<size_t>(1, out.outSamples)));
  };

  double priorBlend = 0.0;
  double priorRms = 0.0;
  for (int block = 0; block < 90; block++) {
    src.fill(iq, 8192);
    dsp.process(iq.data(), 8192, nullptr, out);
    if (block >= 80) {
      priorBlend += out.stereoBlend / 10.0;
      priorRms += blockRms() / 10.0;
    }
  }
  REQUIRE(out.stereoDetected);

  for (int bandwidth : {142000, 194000, 95000}) {
    dsp.setBandwidthHz(bandwidth);
    double minBlend = 1.0;
    double minRms = 1e9;
    bool stereoHeld = true;
    for (int block = 0; block < 30; block++) {
      src.fill(iq, 8192);
      dsp.process(iq.data(), 8192, nullptr, out);
      minBlend = std::min(minBlend, static_cast<double>(out.stereoBlend));
      minRms = std::min(minRms, blockRms());
      stereoHeld = stereoHeld && out.stereoDetected;
    }
    INFO("bandwidth " << bandwidth << ": blend min " << minBlend << " (prior "
                      << priorBlend << "), rms min " << minRms << " (prior "
                      << priorRms << ")");
    CHECK(stereoHeld);
    CHECK(minBlend >= priorBlend - 0.05);
    // 95 kHz trims the L-R subcarrier (23-53 kHz) itself, which lowers a
    // left-only signal for real; the level check is for the wide filters.
    if (bandwidth >= 142000) {
      CHECK(20.0 * std::log10(minRms / priorRms) >= -1.0);
    }
  }
}
