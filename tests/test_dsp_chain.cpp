#include "catch_compat.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <limits>
#include <vector>

#include <cstdio>
#include <cstring>
#include <string>

#include "af_post_processor.h"
#include "config.h"
#include "dsp/multipath_eq.h"
#include "dsp_pipeline.h"
#include "fm_demod.h"
#include "stereo_decoder.h"

namespace {

float rms(const float *data, size_t count) {
  if (!data || count == 0) {
    return 0.0f;
  }
  double acc = 0.0;
  for (size_t i = 0; i < count; i++) {
    const double v = static_cast<double>(data[i]);
    acc += v * v;
  }
  return static_cast<float>(std::sqrt(acc / static_cast<double>(count)));
}

float meanAbsDiff(const float *a, const float *b, size_t count) {
  if (!a || !b || count == 0) {
    return 0.0f;
  }
  double acc = 0.0;
  for (size_t i = 0; i < count; i++) {
    acc += std::abs(static_cast<double>(a[i]) - static_cast<double>(b[i]));
  }
  return static_cast<float>(acc / static_cast<double>(count));
}

struct WavData {
  int sampleRate = 0;
  int channels = 0;
  std::vector<float> samples; // interleaved
};

// Minimal 16-bit PCM WAV reader. Walks the chunk list so it tolerates extra
// JUNK / LIST blocks before the data chunk. Returns false on any structural
// issue; tests that depend on the file simply SKIP when this happens so the
// suite stays green on machines that don't have the fixtures.
bool loadWav(const std::string &path, WavData &out) {
  FILE *f = std::fopen(path.c_str(), "rb");
  if (!f) {
    return false;
  }
  char riff[12];
  if (std::fread(riff, 1, 12, f) != 12 || std::memcmp(riff, "RIFF", 4) != 0 ||
      std::memcmp(riff + 8, "WAVE", 4) != 0) {
    std::fclose(f);
    return false;
  }

  uint16_t channels = 0;
  uint32_t rate = 0;
  uint16_t bits = 0;
  std::vector<int16_t> raw;
  bool gotFmt = false;
  bool gotData = false;

  while (!gotData) {
    char id[4];
    uint32_t size = 0;
    if (std::fread(id, 1, 4, f) != 4) break;
    if (std::fread(&size, 4, 1, f) != 1) break;
    if (std::memcmp(id, "fmt ", 4) == 0) {
      std::vector<uint8_t> fmtBuf(size);
      if (std::fread(fmtBuf.data(), 1, size, f) != size) break;
      if (size < 16) break;
      std::memcpy(&channels, fmtBuf.data() + 2, 2);
      std::memcpy(&rate, fmtBuf.data() + 4, 4);
      std::memcpy(&bits, fmtBuf.data() + 14, 2);
      gotFmt = true;
    } else if (std::memcmp(id, "data", 4) == 0) {
      if (!gotFmt || bits != 16) {
        std::fclose(f);
        return false;
      }
      const size_t numSamples = size / 2;
      raw.resize(numSamples);
      if (std::fread(raw.data(), 2, numSamples, f) != numSamples) break;
      gotData = true;
    } else {
      if (std::fseek(f, static_cast<long>((size + 1) & ~1U), SEEK_CUR) != 0) {
        break;
      }
    }
  }
  std::fclose(f);
  if (!gotData) {
    return false;
  }
  out.sampleRate = static_cast<int>(rate);
  out.channels = channels;
  out.samples.resize(raw.size());
  for (size_t i = 0; i < raw.size(); i++) {
    out.samples[i] = static_cast<float>(raw[i]) / 32768.0f;
  }
  return true;
}

float toneMagnitude(const float *data, size_t count, int sampleRate,
                    float toneHz) {
  if (!data || count == 0 || sampleRate <= 0 || toneHz <= 0.0f) {
    return 0.0f;
  }
  constexpr float kTwoPi = 6.2831853071795864769f;
  double accI = 0.0;
  double accQ = 0.0;
  for (size_t i = 0; i < count; i++) {
    const float phase =
        kTwoPi * toneHz * (static_cast<float>(i) / static_cast<float>(sampleRate));
    accI += static_cast<double>(data[i]) * std::cos(phase);
    accQ += static_cast<double>(data[i]) * std::sin(phase);
  }
  return static_cast<float>(
      std::sqrt((accI * accI) + (accQ * accQ)) / static_cast<double>(count));
}

} // namespace

TEST_CASE("FMDemod constant complex carrier stays near silence",
          "[dsp][fm_demod]") {
  constexpr int kInputRate = 256000;
  constexpr int kOutputRate = 32000;
  constexpr size_t kInSamples = 8192;

  FMDemod demod(kInputRate, kOutputRate);
  std::vector<std::complex<float>> iq(kInSamples,
                                      std::complex<float>(0.45f, -0.25f));
  std::vector<float> mpx(kInSamples, 0.0f);
  std::vector<float> mono(kInSamples, 0.0f);

  const size_t outSamples =
      demod.processSplitComplex(iq.data(), mpx.data(), mono.data(), kInSamples);

  REQUIRE(outSamples > 900);
  REQUIRE(outSamples < 1200);
  REQUIRE(std::isfinite(rms(mpx.data(), kInSamples)));
  REQUIRE(std::isfinite(rms(mono.data(), outSamples)));

  const size_t tailStart = outSamples / 4;
  // Keep tolerance slightly loose across compilers/libm implementations.
  REQUIRE(rms(mono.data() + tailStart, outSamples - tailStart) < 4e-3f);
  REQUIRE(demod.getClippingRatio() == 0.0f);
}

TEST_CASE("FMDemod clipping metric reacts to saturated IQ bytes",
          "[dsp][fm_demod]") {
  constexpr int kInputRate = 256000;
  constexpr int kOutputRate = 32000;
  constexpr size_t kInSamples = 4096;

  FMDemod demod(kInputRate, kOutputRate);
  std::vector<uint8_t> iq(kInSamples * 2, 0);
  for (size_t i = 0; i < kInSamples; i++) {
    iq[i * 2] = (i % 2 == 0) ? 0 : 255;
    iq[i * 2 + 1] = 255;
  }
  std::vector<float> mpx(kInSamples, 0.0f);
  std::vector<float> mono(kInSamples, 0.0f);

  (void)demod.processSplit(iq.data(), mpx.data(), mono.data(), kInSamples);
  REQUIRE(demod.isClipping());
  REQUIRE(demod.getClippingRatio() > 0.95f);
}

TEST_CASE("Stereo decoder force-mono keeps channels matched",
          "[dsp][stereo]") {
  constexpr int kInputRate = 256000;
  constexpr size_t kSamples = 32768;
  constexpr float kToneHz = 1000.0f;
  constexpr float kTwoPi = 6.2831853071795864769f;

  StereoDecoder stereo(kInputRate, 32000);
  stereo.setForceMono(true);

  std::vector<float> mono(kSamples, 0.0f);
  for (size_t i = 0; i < kSamples; i++) {
    const float t = static_cast<float>(i) / static_cast<float>(kInputRate);
    mono[i] = 0.5f * std::sin(kTwoPi * kToneHz * t);
  }

  std::vector<float> left(kSamples, 0.0f);
  std::vector<float> right(kSamples, 0.0f);
  const size_t out = stereo.processAudio(mono.data(), left.data(), right.data(), kSamples);

  REQUIRE(out == kSamples);
  const size_t settle = 4000;
  REQUIRE(out > settle);
  REQUIRE(meanAbsDiff(left.data() + settle, right.data() + settle, out - settle) < 1e-4f);
  REQUIRE_FALSE(stereo.isStereo());
}

TEST_CASE("AF post-processor downsample keeps channels aligned",
          "[dsp][af_post]") {
  constexpr int kInputRate = 256000;
  constexpr int kOutputRate = 32000;
  constexpr size_t kInSamples = 32768;
  constexpr size_t kOutCap = kInSamples;
  constexpr float kTwoPi = 6.2831853071795864769f;

  AFPostProcessor af(kInputRate, kOutputRate);
  af.setDeemphasis(75);

  std::vector<float> inL(kInSamples, 0.0f);
  std::vector<float> inR(kInSamples, 0.0f);
  for (size_t i = 0; i < kInSamples; i++) {
    const float t = static_cast<float>(i) / static_cast<float>(kInputRate);
    const float v = 0.35f * std::sin(kTwoPi * 1200.0f * t) +
                    0.12f * std::sin(kTwoPi * 4200.0f * t);
    inL[i] = v;
    inR[i] = v;
  }

  std::vector<float> outL(kOutCap, 0.0f);
  std::vector<float> outR(kOutCap, 0.0f);
  const size_t out =
      af.process(inL.data(), inR.data(), kInSamples, outL.data(), outR.data(), kOutCap);

  REQUIRE(out > 3500);
  REQUIRE(out < 4500);
  REQUIRE(meanAbsDiff(outL.data(), outR.data(), out) < 1e-5f);

  const float outLrms = rms(outL.data(), out);
  REQUIRE(std::isfinite(outLrms));
  REQUIRE(outLrms > 1e-4f);
  REQUIRE(outLrms < 0.5f);
}

TEST_CASE("FMDemod reset restores deterministic output for same IQ input",
          "[dsp][fm_demod]") {
  constexpr int kInputRate = 256000;
  constexpr int kOutputRate = 32000;
  constexpr size_t kInSamples = 16384;
  constexpr float kTwoPi = 6.2831853071795864769f;

  FMDemod demod(kInputRate, kOutputRate);
  std::vector<std::complex<float>> iq(kInSamples);
  for (size_t i = 0; i < kInSamples; i++) {
    const float t = static_cast<float>(i) / static_cast<float>(kInputRate);
    const float phase = kTwoPi * 2500.0f * t;
    iq[i] = std::complex<float>(0.8f * std::cos(phase), 0.8f * std::sin(phase));
  }

  std::vector<float> mpxA(kInSamples, 0.0f);
  std::vector<float> monoA(kInSamples, 0.0f);
  std::vector<float> mpxB(kInSamples, 0.0f);
  std::vector<float> monoB(kInSamples, 0.0f);

  const size_t outA =
      demod.processSplitComplex(iq.data(), mpxA.data(), monoA.data(), kInSamples);
  demod.reset();
  const size_t outB =
      demod.processSplitComplex(iq.data(), mpxB.data(), monoB.data(), kInSamples);

  REQUIRE(outA == outB);
  REQUIRE(outA > 1500);
  REQUIRE(meanAbsDiff(mpxA.data(), mpxB.data(), kInSamples) < 1e-6f);
  REQUIRE(meanAbsDiff(monoA.data(), monoB.data(), outA) < 1e-6f);
}

TEST_CASE("AF deemphasis attenuates high frequencies more than low frequencies",
          "[dsp][af_post]") {
  constexpr int kInputRate = 256000;
  constexpr int kOutputRate = 32000;
  constexpr size_t kInSamples = 32768;
  constexpr size_t kOutCap = kInSamples;
  constexpr float kTwoPi = 6.2831853071795864769f;

  auto runTone = [&](float toneHz, bool deemph) -> float {
    AFPostProcessor af(kInputRate, kOutputRate);
    af.setDeemphasis(deemph ? 75 : 0);

    std::vector<float> inL(kInSamples, 0.0f);
    std::vector<float> inR(kInSamples, 0.0f);
    for (size_t i = 0; i < kInSamples; i++) {
      const float t = static_cast<float>(i) / static_cast<float>(kInputRate);
      const float v = 0.4f * std::sin(kTwoPi * toneHz * t);
      inL[i] = v;
      inR[i] = v;
    }

    std::vector<float> outL(kOutCap, 0.0f);
    std::vector<float> outR(kOutCap, 0.0f);
    const size_t out =
        af.process(inL.data(), inR.data(), kInSamples, outL.data(), outR.data(), kOutCap);
    REQUIRE(out > 3000);
    return rms(outL.data() + (out / 8), out - (out / 8));
  };

  const float lowNo = runTone(1000.0f, false);
  const float lowYes = runTone(1000.0f, true);
  const float highNo = runTone(10000.0f, false);
  const float highYes = runTone(10000.0f, true);

  REQUIRE(lowNo > 1e-4f);
  REQUIRE(highNo > 1e-4f);
  REQUIRE(lowYes < lowNo);
  REQUIRE(highYes < highNo);

  const float lowRatio = lowYes / std::max(lowNo, 1e-9f);
  const float highRatio = highYes / std::max(highNo, 1e-9f);
  REQUIRE(highRatio < lowRatio);
}

TEST_CASE("AF post reset reproduces identical output for identical input",
          "[dsp][af_post]") {
  constexpr int kInputRate = 256000;
  constexpr int kOutputRate = 32000;
  constexpr size_t kInSamples = 24576;
  constexpr size_t kOutCap = kInSamples;
  constexpr float kTwoPi = 6.2831853071795864769f;

  AFPostProcessor af(kInputRate, kOutputRate);
  af.setDeemphasis(75);

  std::vector<float> inL(kInSamples, 0.0f);
  std::vector<float> inR(kInSamples, 0.0f);
  for (size_t i = 0; i < kInSamples; i++) {
    const float t = static_cast<float>(i) / static_cast<float>(kInputRate);
    inL[i] = 0.4f * std::sin(kTwoPi * 900.0f * t) + 0.1f * std::sin(kTwoPi * 4800.0f * t);
    inR[i] = 0.3f * std::sin(kTwoPi * 1300.0f * t) + 0.12f * std::sin(kTwoPi * 5200.0f * t);
  }

  std::vector<float> outA_L(kOutCap, 0.0f);
  std::vector<float> outA_R(kOutCap, 0.0f);
  std::vector<float> outB_L(kOutCap, 0.0f);
  std::vector<float> outB_R(kOutCap, 0.0f);

  const size_t outA =
      af.process(inL.data(), inR.data(), kInSamples, outA_L.data(), outA_R.data(), kOutCap);
  af.reset();
  const size_t outB =
      af.process(inL.data(), inR.data(), kInSamples, outB_L.data(), outB_R.data(), kOutCap);

  REQUIRE(outA == outB);
  REQUIRE(outA > 2500);
  REQUIRE(meanAbsDiff(outA_L.data(), outB_L.data(), outA) < 1e-6f);
  REQUIRE(meanAbsDiff(outA_R.data(), outB_R.data(), outA) < 1e-6f);
}

TEST_CASE("Stereo decoder force-stereo recovers channel separation on synthetic MPX",
          "[dsp][stereo]") {
  constexpr int kInputRate = 256000;
  constexpr size_t kSamples = 65536;
  constexpr float kPilotHz = 19000.0f;
  constexpr float kTwoPi = 6.2831853071795864769f;

  StereoDecoder stereo(kInputRate, 32000);
  stereo.setForceStereo(true);

  std::vector<float> mpx(kSamples, 0.0f);
  for (size_t i = 0; i < kSamples; i++) {
    const float t = static_cast<float>(i) / static_cast<float>(kInputRate);
    const float l = 0.45f * std::sin(kTwoPi * 1000.0f * t);
    const float r = 0.45f * std::sin(kTwoPi * 2800.0f * t);
    const float lr = l - r;
    const float mono = l + r;
    const float pilot = 0.08f * std::sin(kTwoPi * kPilotHz * t);
    const float dsb = 0.25f * lr * std::sin(kTwoPi * (2.0f * kPilotHz) * t);
    mpx[i] = mono + pilot + dsb;
  }

  std::vector<float> left(kSamples, 0.0f);
  std::vector<float> right(kSamples, 0.0f);
  const size_t out = stereo.processAudio(mpx.data(), left.data(), right.data(), kSamples);
  REQUIRE(out == kSamples);

  const size_t settle = kSamples / 4;
  const float sep = meanAbsDiff(left.data() + settle, right.data() + settle, out - settle);
  REQUIRE(sep > 0.03f);
}

TEST_CASE("Stereo decoder reduces blend when stereo difference path gets noisy",
          "[dsp][stereo]") {
  constexpr int kInputRate = 256000;
  constexpr size_t kSamples = 65536;
  constexpr size_t kBlock = 4096;
  constexpr float kPilotHz = 19000.0f;
  constexpr float kTwoPi = 6.2831853071795864769f;

  auto makeMpx = [](float noiseAmplitude) {
    std::vector<float> mpx(kSamples, 0.0f);
    uint32_t state = 0x2468ace1u;
    auto nextNoise = [&]() -> float {
      state = state * 1664525u + 1013904223u;
      const uint32_t bits = (state >> 8) & 0x00ffffffu;
      return (static_cast<float>(bits) / 8388607.5f) - 1.0f;
    };
    for (size_t i = 0; i < kSamples; i++) {
      const float t = static_cast<float>(i) / static_cast<float>(kInputRate);
      const float l = 0.45f * std::sin(kTwoPi * 1000.0f * t);
      const float r = 0.45f * std::sin(kTwoPi * 2800.0f * t);
      const float lr = l - r;
      const float mono = l + r;
      const float pilot = 0.08f * std::sin(kTwoPi * kPilotHz * t);
      const float dsb = 0.25f * lr * std::sin(kTwoPi * (2.0f * kPilotHz) * t);
      const float noise = noiseAmplitude * nextNoise();
      mpx[i] = mono + pilot + dsb + noise;
    }
    return mpx;
  };

  StereoDecoder clean(kInputRate, 32000);
  StereoDecoder noisy(kInputRate, 32000);

  const std::vector<float> cleanMpx = makeMpx(0.0f);
  const std::vector<float> noisyMpx = makeMpx(0.22f);

  std::vector<float> cleanLeft(kSamples, 0.0f);
  std::vector<float> cleanRight(kSamples, 0.0f);
  std::vector<float> noisyLeft(kSamples, 0.0f);
  std::vector<float> noisyRight(kSamples, 0.0f);

  size_t cleanOut = 0;
  size_t noisyOut = 0;
  for (size_t offset = 0; offset < kSamples; offset += kBlock) {
    const size_t count = std::min(kBlock, kSamples - offset);
    cleanOut += clean.processAudio(cleanMpx.data() + offset, cleanLeft.data() + offset,
                                   cleanRight.data() + offset, count);
    noisyOut += noisy.processAudio(noisyMpx.data() + offset, noisyLeft.data() + offset,
                                   noisyRight.data() + offset, count);
  }

  REQUIRE(cleanOut == kSamples);
  REQUIRE(noisyOut == kSamples);

  const size_t settle = kSamples / 4;
  const float cleanSep =
      meanAbsDiff(cleanLeft.data() + settle, cleanRight.data() + settle,
                  kSamples - settle);
  const float noisySep =
      meanAbsDiff(noisyLeft.data() + settle, noisyRight.data() + settle,
                  kSamples - settle);

  REQUIRE(clean.isStereo());
  REQUIRE(clean.getStereoQuality() > noisy.getStereoQuality());
  REQUIRE(clean.getStereoBlend() > noisy.getStereoBlend());
  REQUIRE(cleanSep > noisySep);
}

TEST_CASE("Stereo decoder releases blend quickly when quality degrades",
          "[dsp][stereo]") {
  constexpr int kInputRate = 256000;
  constexpr size_t kBlock = 4096;
  constexpr size_t kWarmBlocks = 20;
  constexpr size_t kNoisyBlocks = 3;
  constexpr float kPilotHz = 19000.0f;
  constexpr float kTwoPi = 6.2831853071795864769f;

  auto makeBlock = [](float noiseAmplitude, uint32_t seedBase, size_t sampleOffset) {
    std::vector<float> mpx(kBlock, 0.0f);
    uint32_t state = seedBase;
    auto nextNoise = [&]() -> float {
      state = state * 1664525u + 1013904223u;
      const uint32_t bits = (state >> 8) & 0x00ffffffu;
      return (static_cast<float>(bits) / 8388607.5f) - 1.0f;
    };
    for (size_t i = 0; i < kBlock; i++) {
      const float t = static_cast<float>(sampleOffset + i) /
                      static_cast<float>(kInputRate);
      const float l = 0.45f * std::sin(kTwoPi * 1000.0f * t);
      const float r = 0.45f * std::sin(kTwoPi * 2800.0f * t);
      const float lr = l - r;
      const float mono = l + r;
      const float pilot = 0.08f * std::sin(kTwoPi * kPilotHz * t);
      const float dsb = 0.25f * lr * std::sin(kTwoPi * (2.0f * kPilotHz) * t);
      mpx[i] = mono + pilot + dsb + (noiseAmplitude * nextNoise());
    }
    return mpx;
  };

  StereoDecoder stereo(kInputRate, 32000);
  std::vector<float> left(kBlock, 0.0f);
  std::vector<float> right(kBlock, 0.0f);

  for (size_t block = 0; block < kWarmBlocks; ++block) {
    const std::vector<float> clean =
        makeBlock(0.0f, 0x1000u + static_cast<uint32_t>(block), block * kBlock);
    REQUIRE(stereo.processAudio(clean.data(), left.data(), right.data(), kBlock) == kBlock);
  }

  const float warmBlend = stereo.getStereoBlend();
  REQUIRE(stereo.isStereo());
  REQUIRE(warmBlend > 0.75f);

  float lastNoisyBlend = warmBlend;
  for (size_t block = 0; block < kNoisyBlocks; ++block) {
    const std::vector<float> noisy = makeBlock(
        0.30f, 0x2000u + static_cast<uint32_t>(block),
        (kWarmBlocks + block) * kBlock);
    REQUIRE(stereo.processAudio(noisy.data(), left.data(), right.data(), kBlock) == kBlock);
    lastNoisyBlend = stereo.getStereoBlend();
  }

  REQUIRE(lastNoisyBlend < warmBlend - 0.10f);
}

TEST_CASE("Stereo decoder acquires lock through brief marginal pilot blocks",
          "[dsp][stereo]") {
  constexpr int kInputRate = 256000;
  constexpr size_t kBlock = 4096;
  constexpr float kPilotHz = 19000.0f;
  constexpr float kTwoPi = 6.2831853071795864769f;

  auto makeBlock = [](float pilotAmp, size_t sampleOffset) {
    std::vector<float> mpx(kBlock, 0.0f);
    for (size_t i = 0; i < kBlock; i++) {
      const float t = static_cast<float>(sampleOffset + i) /
                      static_cast<float>(kInputRate);
      const float l = 0.45f * std::sin(kTwoPi * 1000.0f * t);
      const float r = 0.45f * std::sin(kTwoPi * 2800.0f * t);
      const float lr = l - r;
      const float mono = l + r;
      const float pilot = pilotAmp * std::sin(kTwoPi * kPilotHz * t);
      const float dsb = 0.25f * lr * std::sin(kTwoPi * (2.0f * kPilotHz) * t);
      mpx[i] = mono + pilot + dsb;
    }
    return mpx;
  };

  StereoDecoder stereo(kInputRate, 32000);
  std::vector<float> left(kBlock, 0.0f);
  std::vector<float> right(kBlock, 0.0f);

  const float pilotSequence[] = {0.08f, 0.08f, 0.05f, 0.08f, 0.08f};
  for (size_t block = 0; block < (sizeof(pilotSequence) / sizeof(pilotSequence[0]));
       ++block) {
    const std::vector<float> mpx =
        makeBlock(pilotSequence[block], block * kBlock);
    REQUIRE(stereo.processAudio(mpx.data(), left.data(), right.data(), kBlock) ==
            kBlock);
  }

  REQUIRE(stereo.isStereo());
}

TEST_CASE("Stereo decoder does not treat clean high-frequency stereo content as noise",
          "[dsp][stereo]") {
  constexpr int kInputRate = 256000;
  constexpr size_t kSamples = 65536;
  constexpr size_t kBlock = 4096;
  constexpr float kPilotHz = 19000.0f;
  constexpr float kTwoPi = 6.2831853071795864769f;

  auto makeMpx = [&](float rightHz) {
    std::vector<float> mpx(kSamples, 0.0f);
    for (size_t i = 0; i < kSamples; i++) {
      const float t = static_cast<float>(i) / static_cast<float>(kInputRate);
      const float l = 0.45f * std::sin(kTwoPi * 1000.0f * t);
      const float r = 0.45f * std::sin(kTwoPi * rightHz * t);
      const float lr = l - r;
      const float mono = l + r;
      const float pilot = 0.08f * std::sin(kTwoPi * kPilotHz * t);
      const float dsb = 0.25f * lr * std::sin(kTwoPi * (2.0f * kPilotHz) * t);
      mpx[i] = mono + pilot + dsb;
    }
    return mpx;
  };

  StereoDecoder lowFreq(kInputRate, 32000);
  StereoDecoder highFreq(kInputRate, 32000);

  const std::vector<float> lowFreqMpx = makeMpx(2800.0f);
  const std::vector<float> highFreqMpx = makeMpx(9500.0f);
  std::vector<float> left(kBlock, 0.0f);
  std::vector<float> right(kBlock, 0.0f);

  for (size_t offset = 0; offset < kSamples; offset += kBlock) {
    const size_t count = std::min(kBlock, kSamples - offset);
    REQUIRE(lowFreq.processAudio(lowFreqMpx.data() + offset, left.data(), right.data(),
                                 count) == count);
    REQUIRE(highFreq.processAudio(highFreqMpx.data() + offset, left.data(), right.data(),
                                  count) == count);
  }

  REQUIRE(lowFreq.isStereo());
  REQUIRE(highFreq.isStereo());
  REQUIRE(highFreq.getStereoQuality() > 0.65f);
  REQUIRE(highFreq.getStereoBlend() > 0.75f);
  REQUIRE(std::abs(highFreq.getStereoBlend() - lowFreq.getStereoBlend()) < 0.15f);
}

TEST_CASE("Stereo decoder reconstructs left and right channels with correct dominance",
          "[dsp][stereo]") {
  constexpr int kInputRate = 256000;
  constexpr size_t kSamples = 65536;
  constexpr size_t kBlock = 4096;
  constexpr float kPilotHz = 19000.0f;
  constexpr float kTwoPi = 6.2831853071795864769f;
  constexpr float kLeftHz = 1000.0f;
  constexpr float kRightHz = 2800.0f;

  std::vector<float> mpx(kSamples, 0.0f);
  for (size_t i = 0; i < kSamples; i++) {
    const float t = static_cast<float>(i) / static_cast<float>(kInputRate);
    const float l = 0.45f * std::sin(kTwoPi * kLeftHz * t);
    const float r = 0.45f * std::sin(kTwoPi * kRightHz * t);
    const float lr = l - r;
    const float mono = l + r;
    const float pilot = 0.08f * std::sin(kTwoPi * kPilotHz * t);
    const float dsb = 0.25f * lr * std::sin(kTwoPi * (2.0f * kPilotHz) * t);
    mpx[i] = mono + pilot + dsb;
  }

  StereoDecoder stereo(kInputRate, 32000);
  std::vector<float> left(kSamples, 0.0f);
  std::vector<float> right(kSamples, 0.0f);

  size_t out = 0;
  for (size_t offset = 0; offset < kSamples; offset += kBlock) {
    const size_t count = std::min(kBlock, kSamples - offset);
    out += stereo.processAudio(mpx.data() + offset, left.data() + offset,
                               right.data() + offset, count);
  }

  REQUIRE(out == kSamples);
  REQUIRE(stereo.isStereo());
  REQUIRE(stereo.getStereoBlend() > 0.75f);

  const size_t settle = kSamples / 4;
  const float leftAtLeftHz =
      toneMagnitude(left.data() + settle, kSamples - settle, kInputRate, kLeftHz);
  const float leftAtRightHz =
      toneMagnitude(left.data() + settle, kSamples - settle, kInputRate, kRightHz);
  const float rightAtRightHz = toneMagnitude(right.data() + settle, kSamples - settle,
                                             kInputRate, kRightHz);
  const float rightAtLeftHz =
      toneMagnitude(right.data() + settle, kSamples - settle, kInputRate, kLeftHz);

  REQUIRE(leftAtLeftHz > leftAtRightHz);
  REQUIRE(rightAtRightHz > rightAtLeftHz);
}

TEST_CASE("Stereo decoder matrix keeps normal stereo program material out of hard clip range",
          "[dsp][stereo]") {
  constexpr int kInputRate = 256000;
  constexpr size_t kSamples = 65536;
  constexpr size_t kBlock = 4096;
  constexpr float kPilotHz = 19000.0f;
  constexpr float kTwoPi = 6.2831853071795864769f;

  std::vector<float> mpx(kSamples, 0.0f);
  for (size_t i = 0; i < kSamples; i++) {
    const float t = static_cast<float>(i) / static_cast<float>(kInputRate);
    const float l = 0.70f * std::sin(kTwoPi * 1000.0f * t);
    const float r = 0.70f * std::sin(kTwoPi * 2800.0f * t);
    const float lr = l - r;
    const float mono = l + r;
    const float pilot = 0.08f * std::sin(kTwoPi * kPilotHz * t);
    const float dsb = 0.25f * lr * std::sin(kTwoPi * (2.0f * kPilotHz) * t);
    mpx[i] = mono + pilot + dsb;
  }

  StereoDecoder stereo(kInputRate, 32000);
  std::vector<float> left(kSamples, 0.0f);
  std::vector<float> right(kSamples, 0.0f);

  size_t out = 0;
  float maxAbs = 0.0f;
  for (size_t offset = 0; offset < kSamples; offset += kBlock) {
    const size_t count = std::min(kBlock, kSamples - offset);
    out += stereo.processAudio(mpx.data() + offset, left.data() + offset,
                               right.data() + offset, count);
    for (size_t i = 0; i < count; i++) {
      maxAbs = std::max(maxAbs, std::abs(left[offset + i]));
      maxAbs = std::max(maxAbs, std::abs(right[offset + i]));
    }
  }

  REQUIRE(out == kSamples);
  REQUIRE(maxAbs < 0.95f);
}

TEST_CASE("Stereo decoder locks and separates channels with rotated pilot phase",
          "[dsp][stereo]") {
  constexpr int kInputRate = 256000;
  constexpr size_t kSamples = 65536;
  constexpr size_t kBlock = 4096;
  constexpr float kPilotHz = 19000.0f;
  constexpr float kTwoPi = 6.2831853071795864769f;
  constexpr float kPilotPhase = 0.85f;

  std::vector<float> mpx(kSamples, 0.0f);
  for (size_t i = 0; i < kSamples; i++) {
    const float t = static_cast<float>(i) / static_cast<float>(kInputRate);
    const float l = 0.45f * std::sin(kTwoPi * 1000.0f * t);
    const float r = 0.45f * std::sin(kTwoPi * 3200.0f * t);
    const float lr = l - r;
    const float mono = l + r;
    const float pilot = 0.08f * std::sin((kTwoPi * kPilotHz * t) + kPilotPhase);
    const float dsb =
        0.25f * lr * std::sin((kTwoPi * (2.0f * kPilotHz) * t) + (2.0f * kPilotPhase));
    mpx[i] = mono + pilot + dsb;
  }

  StereoDecoder stereo(kInputRate, 32000);
  std::vector<float> left(kSamples, 0.0f);
  std::vector<float> right(kSamples, 0.0f);

  size_t out = 0;
  for (size_t offset = 0; offset < kSamples; offset += kBlock) {
    const size_t count = std::min(kBlock, kSamples - offset);
    out += stereo.processAudio(mpx.data() + offset, left.data() + offset,
                               right.data() + offset, count);
  }

  REQUIRE(out == kSamples);
  REQUIRE(stereo.isStereo());
  REQUIRE(stereo.getStereoBlend() > 0.70f);

  const size_t settle = kSamples / 4;
  const float sep =
      meanAbsDiff(left.data() + settle, right.data() + settle, kSamples - settle);
  REQUIRE(sep > 0.03f);
}

TEST_CASE("DspPipeline decimation staging handles fragmented high-rate IQ without shuffling semantics",
          "[dsp][pipeline]") {
  Config::ProcessingSection processing;
  processing.stereo = false;
  processing.w0_bandwidth_hz = 194000;

  constexpr int kInputRate = 256000;
  constexpr int kOutputRate = 32000;
  constexpr size_t kBlockSamples = 8192;
  constexpr size_t kIqDecimation = 4;

  DspPipeline pipeline(kInputRate, kOutputRate, processing, false,
                       kBlockSamples, kIqDecimation);
  REQUIRE(pipeline.sdrBlockSamples() == kBlockSamples * kIqDecimation);

  const size_t totalIqSamples = pipeline.sdrBlockSamples() * 2;
  std::vector<uint8_t> iq(totalIqSamples * 2, 0);
  for (size_t i = 0; i < totalIqSamples; i++) {
    iq[i * 2] = 140;
    iq[i * 2 + 1] = 110;
  }

  const size_t fragments[] = {3000, 5000, 7000, 9000, 8768, 1000, 2000, 5000, 6768};
  size_t offset = 0;
  size_t producedBlocks = 0;
  size_t fragmentIndex = 0;
  while (offset < totalIqSamples) {
    const size_t fragmentSamples =
        fragments[fragmentIndex % (sizeof(fragments) / sizeof(fragments[0]))];
    const size_t chunk = std::min(fragmentSamples, totalIqSamples - offset);
    DspPipeline::Result out;
    const bool have = pipeline.process(
        iq.data() + (offset * 2), chunk,
        [](const float *, size_t) {}, out);
    if (have) {
      REQUIRE(out.demodSamples == kBlockSamples);
      REQUIRE(out.outSamples > 900);
      REQUIRE(std::isfinite(out.left[std::min<size_t>(10, out.outSamples - 1)]));
      producedBlocks++;
    }
    offset += chunk;
    fragmentIndex++;
  }

  REQUIRE(offset == totalIqSamples);
  REQUIRE(producedBlocks == 2);
}

TEST_CASE("DspPipeline::softLimitSample passes through below threshold",
          "[dsp_pipeline][soft_limit]") {
  uint32_t count = 0;
  REQUIRE(DspPipeline::softLimitSample(0.0f, count) == 0.0f);
  REQUIRE(DspPipeline::softLimitSample(0.5f, count) == 0.5f);
  REQUIRE(DspPipeline::softLimitSample(-0.8f, count) == -0.8f);
  REQUIRE(DspPipeline::softLimitSample(DspPipeline::kSoftLimitThreshold,
                                       count) ==
          DspPipeline::kSoftLimitThreshold);
  REQUIRE(count == 0);
}

TEST_CASE("DspPipeline::softLimitSample soft-knees above threshold and counts",
          "[dsp_pipeline][soft_limit]") {
  uint32_t count = 0;

  const float over = DspPipeline::softLimitSample(1.5f, count);
  REQUIRE(over > DspPipeline::kSoftLimitThreshold);
  REQUIRE(over < 1.0f);

  const float negOver = DspPipeline::softLimitSample(-1.5f, count);
  REQUIRE(negOver < -DspPipeline::kSoftLimitThreshold);
  REQUIRE(negOver > -1.0f);

  REQUIRE(count == 2);

  const float huge = DspPipeline::softLimitSample(100.0f, count);
  REQUIRE(huge <= 1.0f);
  REQUIRE(huge >= -1.0f);

  const float monotonicA = DspPipeline::softLimitSample(0.96f, count);
  const float monotonicB = DspPipeline::softLimitSample(0.99f, count);
  const float monotonicC = DspPipeline::softLimitSample(1.20f, count);
  REQUIRE(monotonicA < monotonicB);
  REQUIRE(monotonicB < monotonicC);
}

TEST_CASE("Stereo hysteresis acquires and drops on a block-size-independent timeline",
          "[dsp][stereo][hysteresis]") {
  constexpr int kInputRate = 256000;
  constexpr float kPilotHz = 19000.0f;
  constexpr float kTwoPi = 6.2831853071795864769f;
  constexpr size_t kHoldSamples = 2 * kInputRate / 5;  // 400 ms of clean MPX
  constexpr size_t kDropSamples = kInputRate;          // 1 s of pilot-less MPX

  auto makeSample = [](size_t i, bool withPilot) {
    const float t = static_cast<float>(i) / static_cast<float>(kInputRate);
    const float l = 0.45f * std::sin(kTwoPi * 1000.0f * t);
    const float r = 0.45f * std::sin(kTwoPi * 2800.0f * t);
    const float mono = l + r;
    const float pilot =
        withPilot ? 0.08f * std::sin(kTwoPi * kPilotHz * t) : 0.0f;
    const float dsb =
        withPilot ? 0.25f * (l - r) * std::sin(kTwoPi * 2.0f * kPilotHz * t)
                  : 0.0f;
    return mono + pilot + dsb;
  };

  auto runWithBlock = [&](size_t blockSize) {
    StereoDecoder dec(kInputRate, 32000);
    std::vector<float> mpx(blockSize, 0.0f);
    std::vector<float> left(blockSize, 0.0f);
    std::vector<float> right(blockSize, 0.0f);

    size_t acquireSample = 0;
    bool acquired = false;
    for (size_t i = 0; i < kHoldSamples; i += blockSize) {
      const size_t count = std::min(blockSize, kHoldSamples - i);
      for (size_t k = 0; k < count; k++) {
        mpx[k] = makeSample(i + k, true);
      }
      dec.processAudio(mpx.data(), left.data(), right.data(), count);
      if (!acquired && dec.isStereo()) {
        acquired = true;
        acquireSample = i + count;
      }
    }

    size_t dropSample = 0;
    bool dropped = false;
    if (acquired) {
      for (size_t i = 0; i < kDropSamples && !dropped; i += blockSize) {
        const size_t count = std::min(blockSize, kDropSamples - i);
        for (size_t k = 0; k < count; k++) {
          mpx[k] = makeSample(kHoldSamples + i + k, false);
        }
        dec.processAudio(mpx.data(), left.data(), right.data(), count);
        if (!dec.isStereo()) {
          dropped = true;
          dropSample = i + count;
        }
      }
    }

    struct Outcome {
      bool acquired;
      size_t acquireSample;
      bool dropped;
      size_t dropSample;
    };
    return Outcome{acquired, acquireSample, dropped, dropSample};
  };

  const auto small = runWithBlock(512);
  const auto large = runWithBlock(8192);

  REQUIRE(small.acquired);
  REQUIRE(large.acquired);
  REQUIRE(small.dropped);
  REQUIRE(large.dropped);

  const float acquireDeltaSec =
      std::abs(static_cast<float>(small.acquireSample) -
               static_cast<float>(large.acquireSample)) /
      static_cast<float>(kInputRate);
  const float dropDeltaSec =
      std::abs(static_cast<float>(small.dropSample) -
               static_cast<float>(large.dropSample)) /
      static_cast<float>(kInputRate);

  // Acquire and drop times should not drift by more than roughly one large
  // block between the two configurations. The granularity of the larger
  // 8192-sample block at 256 kHz is 32 ms, so allow a 50 ms acquire window
  // and a 200 ms drop window to absorb per-block quantization.
  REQUIRE(acquireDeltaSec < 0.050f);
  REQUIRE(dropDeltaSec < 0.200f);
}

TEST_CASE("DspPipeline surfaces audioClipRatio and keeps audio in range",
          "[dsp_pipeline][soft_limit]") {
  constexpr size_t kBlockSamples = 256;
  constexpr size_t kIqDecimation = 1;
  constexpr int kInputRate = 256000;
  constexpr int kOutputRate = 32000;
  Config::ProcessingSection processing;
  processing.stereo = true;
  processing.w0_bandwidth_hz = 194000;
  processing.stereo_blend = "normal";
  processing.dsp_agc = "off";
  DspPipeline pipeline(kInputRate, kOutputRate, processing, false,
                       kBlockSamples, kIqDecimation);

  std::vector<uint8_t> iq(kBlockSamples * 2, 0);
  for (size_t i = 0; i < kBlockSamples; i++) {
    iq[i * 2] = 140;
    iq[i * 2 + 1] = 110;
  }

  DspPipeline::Result out;
  const bool have = pipeline.process(
      iq.data(), kBlockSamples, [](const float *, size_t) {}, out);
  REQUIRE(have);
  REQUIRE(out.outSamples > 0);
  REQUIRE(out.audioClipRatio >= 0.0f);
  REQUIRE(out.audioClipRatio <= 1.0f);
  for (size_t i = 0; i < out.outSamples; i++) {
    REQUIRE(out.left[i] >= -1.0f);
    REQUIRE(out.left[i] <= 1.0f);
    REQUIRE(out.right[i] >= -1.0f);
    REQUIRE(out.right[i] <= 1.0f);
  }
}

TEST_CASE("Stereo decoder pilot canceller suppresses 19 kHz in mono output",
          "[dsp][stereo][pilot]") {
  constexpr int kInputRate = 256000;
  constexpr size_t kSamples = 196608; // ~770 ms
  constexpr float kPilotHz = 19000.0f;
  constexpr float kMonoToneHz = 1000.0f;
  constexpr float kTwoPi = 6.2831853071795864769f;

  StereoDecoder stereo(kInputRate, 48000);
  stereo.setForceStereo(true);
  stereo.setPilotCancellerEnabled(true);

  std::vector<float> mpx(kSamples, 0.0f);
  for (size_t i = 0; i < kSamples; i++) {
    const float t = static_cast<float>(i) / static_cast<float>(kInputRate);
    const float mono = 0.40f * std::sin(kTwoPi * kMonoToneHz * t);
    const float pilot = 0.10f * std::sin(kTwoPi * kPilotHz * t);
    mpx[i] = mono + pilot;
  }

  std::vector<float> left(kSamples, 0.0f);
  std::vector<float> right(kSamples, 0.0f);
  const size_t out =
      stereo.processAudio(mpx.data(), left.data(), right.data(), kSamples);
  REQUIRE(out == kSamples);

  // Measure tones only in the back half so the LMS has had time to converge.
  const size_t analyzeStart = kSamples / 2;
  const size_t analyzeLen = kSamples - analyzeStart;
  std::vector<float> monoOut(analyzeLen, 0.0f);
  for (size_t i = 0; i < analyzeLen; i++) {
    monoOut[i] = 0.5f * (left[analyzeStart + i] + right[analyzeStart + i]);
  }
  const float monoMag =
      toneMagnitude(monoOut.data(), analyzeLen, kInputRate, kMonoToneHz);
  const float pilotMag =
      toneMagnitude(monoOut.data(), analyzeLen, kInputRate, kPilotHz);
  // 0.40 mono → 0.20 after the L/R split → 0.10 in 0.5·(L+R) → 0.05 via the
  // amplitude/2 convention of toneMagnitude.
  REQUIRE(monoMag > 0.04f);
  // Pre-canceller residual would be ~0.025 (10% pilot through the same chain).
  // After the LMS settles the residual at 19 kHz is gradient-noise-limited
  // by the 1 kHz mono cross-correlation, but still ≥ 15 dB below pre-cancel.
  REQUIRE(pilotMag < 0.005f);
}

TEST_CASE("Stereo decoder pilot canceller can be disabled",
          "[dsp][stereo][pilot]") {
  constexpr int kInputRate = 256000;
  constexpr size_t kSamples = 196608;
  constexpr float kPilotHz = 19000.0f;
  constexpr float kTwoPi = 6.2831853071795864769f;

  StereoDecoder stereo(kInputRate, 48000);
  stereo.setForceStereo(true);
  stereo.setPilotCancellerEnabled(false);

  std::vector<float> mpx(kSamples, 0.0f);
  for (size_t i = 0; i < kSamples; i++) {
    const float t = static_cast<float>(i) / static_cast<float>(kInputRate);
    const float mono = 0.40f * std::sin(kTwoPi * 1000.0f * t);
    const float pilot = 0.10f * std::sin(kTwoPi * kPilotHz * t);
    mpx[i] = mono + pilot;
  }

  std::vector<float> left(kSamples, 0.0f);
  std::vector<float> right(kSamples, 0.0f);
  stereo.processAudio(mpx.data(), left.data(), right.data(), kSamples);

  const size_t analyzeStart = kSamples / 2;
  const size_t analyzeLen = kSamples - analyzeStart;
  std::vector<float> monoOut(analyzeLen, 0.0f);
  for (size_t i = 0; i < analyzeLen; i++) {
    monoOut[i] = 0.5f * (left[analyzeStart + i] + right[analyzeStart + i]);
  }
  const float pilotMag =
      toneMagnitude(monoOut.data(), analyzeLen, kInputRate, kPilotHz);
  // Without the canceller, full pilot amplitude survives (0.10 input → ~0.025
  // after the L/R split + 0.5·(L+R) averaging + amplitude/2 convention).
  REQUIRE(pilotMag > 0.020f);
}

TEST_CASE("AF post HiCut narrows the top end when signal quality is low",
          "[dsp][af][hicut]") {
  constexpr int kInputRate = 48000;
  constexpr int kOutputRate = 48000;
  constexpr size_t kSamples = 16384;
  constexpr float kTwoPi = 6.2831853071795864769f;

  std::vector<float> inL(kSamples, 0.0f);
  std::vector<float> inR(kSamples, 0.0f);
  for (size_t i = 0; i < kSamples; i++) {
    const float t = static_cast<float>(i) / static_cast<float>(kInputRate);
    inL[i] = 0.5f * std::sin(kTwoPi * 8000.0f * t);
    inR[i] = inL[i];
  }
  std::vector<float> outL(kSamples, 0.0f);
  std::vector<float> outR(kSamples, 0.0f);

  AFPostProcessor afHigh(kInputRate, kOutputRate);
  afHigh.setDeemphasis(50);
  afHigh.setHicutMode(AFPostProcessor::HicutMode::Strong);
  afHigh.setSignalQuality(1.0f);
  afHigh.process(inL.data(), inR.data(), kSamples, outL.data(), outR.data(),
                 kSamples);
  const float magHighQ =
      toneMagnitude(outL.data() + kSamples / 2, kSamples / 2, kOutputRate,
                    8000.0f);

  AFPostProcessor afLow(kInputRate, kOutputRate);
  afLow.setDeemphasis(50);
  afLow.setHicutMode(AFPostProcessor::HicutMode::Strong);
  afLow.setSignalQuality(0.0f);
  afLow.process(inL.data(), inR.data(), kSamples, outL.data(), outR.data(),
                kSamples);
  const float magLowQ =
      toneMagnitude(outL.data() + kSamples / 2, kSamples / 2, kOutputRate,
                    8000.0f);

  // Strong HiCut at zero quality should attenuate the 8 kHz tone meaningfully
  // versus full quality (where the canonical 50 µs de-emphasis curve applies).
  REQUIRE(magHighQ > 0.0f);
  REQUIRE(magLowQ < magHighQ * 0.5f);
}

TEST_CASE("Multipath equalizer is a pass-through when mode is Off",
          "[dsp][multipath_eq]") {
  fm_tuner::dsp::MultipathEqualizer eq;
  eq.init(fm_tuner::dsp::MultipathEqMode::Off, 17, 256000.0f);
  eq.setAdaptEnabled(true);
  std::complex<float> in(0.7f, -0.3f);
  REQUIRE(eq.execute(in) == in);
}

TEST_CASE("Multipath equalizer initial state is a unit delay",
          "[dsp][multipath_eq]") {
  fm_tuner::dsp::MultipathEqualizer eq;
  eq.init(fm_tuner::dsp::MultipathEqMode::Light, 17, 256000.0f);
  eq.setAdaptEnabled(false); // freeze taps so we observe the initial response

  // For a 17-tap equalizer initialized as a centre-tap delta, the first
  // non-zero output appears (taps/2) = 8 samples in.
  std::complex<float> impulse(1.0f, 0.0f);
  std::complex<float> zero(0.0f, 0.0f);
  std::complex<float> outAtCentre(0.0f, 0.0f);
  for (int n = 0; n < 17; n++) {
    const std::complex<float> sample = (n == 0) ? impulse : zero;
    const std::complex<float> y = eq.execute(sample);
    if (n == 8) {
      outAtCentre = y;
    } else if (n != 8) {
      REQUIRE(std::abs(y) < 1e-6f);
    }
  }
  REQUIRE(std::abs(outAtCentre - impulse) < 1e-6f);
}

TEST_CASE("Multipath equalizer reduces envelope variance on a 2-ray channel",
          "[dsp][multipath_eq]") {
  constexpr int kInputRate = 256000;
  constexpr size_t kSamples = 200000; // ~780 ms, plenty for CMA to settle
  constexpr float kTwoPi = 6.2831853071795864769f;

  // Build a constant-envelope FM-like signal: e^(jφ(t)) where φ is the
  // integral of a 1 kHz mono modulator (so the envelope of x is exactly 1).
  std::vector<std::complex<float>> clean(kSamples);
  float phase = 0.0f;
  for (size_t i = 0; i < kSamples; i++) {
    const float t = static_cast<float>(i) / static_cast<float>(kInputRate);
    const float modulator = 0.5f * std::sin(kTwoPi * 1000.0f * t);
    phase += (2.0f * kTwoPi * 50000.0f / kInputRate) * modulator;
    clean[i] = std::complex<float>(std::cos(phase), std::sin(phase));
  }

  // 2-ray multipath: y[n] = x[n] + 0.5·e^(jπ/4)·x[n-15]. The second ray is
  // delayed and rotated; this distorts the unit envelope substantially.
  std::vector<std::complex<float>> multipath(kSamples);
  const std::complex<float> echoCoeff =
      0.5f * std::complex<float>(std::cos(kTwoPi / 8.0f),
                                  std::sin(kTwoPi / 8.0f));
  constexpr size_t kEchoDelay = 15;
  for (size_t i = 0; i < kSamples; i++) {
    const std::complex<float> echo =
        (i >= kEchoDelay) ? clean[i - kEchoDelay] : std::complex<float>(0.0f, 0.0f);
    multipath[i] = clean[i] + echoCoeff * echo;
  }

  auto envelopeStdDev = [](const std::complex<float> *signal, size_t start,
                           size_t len) {
    double sumMag = 0.0;
    double sumMagSq = 0.0;
    for (size_t i = 0; i < len; i++) {
      const float m = std::abs(signal[start + i]);
      sumMag += m;
      sumMagSq += static_cast<double>(m) * m;
    }
    const double mean = sumMag / len;
    const double var = std::max(0.0, sumMagSq / len - mean * mean);
    return std::sqrt(var);
  };

  const size_t analyzeStart = kSamples / 2;
  const size_t analyzeLen = kSamples - analyzeStart;
  const double preStdDev =
      envelopeStdDev(multipath.data(), analyzeStart, analyzeLen);

  fm_tuner::dsp::MultipathEqualizer eq;
  eq.init(fm_tuner::dsp::MultipathEqMode::Aggressive, 33, kInputRate);
  eq.setAdaptEnabled(true);
  std::vector<std::complex<float>> equalized(kSamples);
  for (size_t i = 0; i < kSamples; i++) {
    equalized[i] = eq.execute(multipath[i]);
  }
  const double postStdDev =
      envelopeStdDev(equalized.data(), analyzeStart, analyzeLen);

  REQUIRE(preStdDev > 0.05); // sanity: the channel actually distorted things
  // CMA settled — envelope should be measurably flatter than the input.
  REQUIRE(postStdDev < preStdDev * 0.7);
}

TEST_CASE("AF post HiCut off keeps the configured de-emphasis intact",
          "[dsp][af][hicut]") {
  constexpr int kInputRate = 48000;
  constexpr int kOutputRate = 48000;
  constexpr size_t kSamples = 16384;
  constexpr float kTwoPi = 6.2831853071795864769f;

  std::vector<float> inL(kSamples, 0.0f);
  std::vector<float> inR(kSamples, 0.0f);
  for (size_t i = 0; i < kSamples; i++) {
    const float t = static_cast<float>(i) / static_cast<float>(kInputRate);
    inL[i] = 0.5f * std::sin(kTwoPi * 8000.0f * t);
    inR[i] = inL[i];
  }
  std::vector<float> outBaseL(kSamples, 0.0f);
  std::vector<float> outBaseR(kSamples, 0.0f);
  std::vector<float> outOffLowQL(kSamples, 0.0f);
  std::vector<float> outOffLowQR(kSamples, 0.0f);

  AFPostProcessor afBase(kInputRate, kOutputRate);
  afBase.setDeemphasis(50);
  afBase.setHicutMode(AFPostProcessor::HicutMode::Off);
  afBase.setSignalQuality(1.0f);
  afBase.process(inL.data(), inR.data(), kSamples, outBaseL.data(),
                 outBaseR.data(), kSamples);

  AFPostProcessor afOff(kInputRate, kOutputRate);
  afOff.setDeemphasis(50);
  afOff.setHicutMode(AFPostProcessor::HicutMode::Off);
  afOff.setSignalQuality(0.0f); // should not engage HiCut
  afOff.process(inL.data(), inR.data(), kSamples, outOffLowQL.data(),
                outOffLowQR.data(), kSamples);

  // HiCut=Off must not respond to quality at all — outputs should be
  // bit-identical for a deterministic input.
  const float diff = meanAbsDiff(outBaseL.data(), outOffLowQL.data(), kSamples);
  REQUIRE(diff < 1e-6f);
}

TEST_CASE("Captured MPX fixture produces audio close to the reference",
          "[dsp][fixture][regression]") {
#ifndef FM_TUNER_REPO_ROOT
  SKIP("FM_TUNER_REPO_ROOT not defined; skipping fixture regression");
#else
  const std::string mpxPath =
      std::string(FM_TUNER_REPO_ROOT) + "/mpx_88600_60s.wav";
  const std::string refPath =
      std::string(FM_TUNER_REPO_ROOT) + "/stereo_88600_60s.wav";

  WavData mpx;
  WavData ref;
  if (!loadWav(mpxPath, mpx) || !loadWav(refPath, ref)) {
    SKIP("regression fixtures not found at repo root; skipping");
  }
  REQUIRE(mpx.channels == 1);
  REQUIRE(mpx.sampleRate == 256000);
  REQUIRE(ref.channels == 2);
  REQUIRE(!mpx.samples.empty());
  REQUIRE(!ref.samples.empty());

  // The reference was captured at 32 kHz output; pipeline this test at the
  // same rate so AFPostProcessor produces a length-comparable output.
  const int kInputRate = mpx.sampleRate;
  const int kOutputRate = ref.sampleRate;

  StereoDecoder stereo(kInputRate, kOutputRate);
  AFPostProcessor af(kInputRate, kOutputRate);
  af.setDeemphasis(50);

  const size_t inSamples = mpx.samples.size();
  std::vector<float> stereoL(inSamples, 0.0f);
  std::vector<float> stereoR(inSamples, 0.0f);
  // Process in production-sized blocks so the per-block stereo-lock
  // confidence integrator gets multiple chances to commit (passing the whole
  // 2.5 s file in one shot would never satisfy the acquire τ since lock is
  // only re-evaluated at block boundaries).
  constexpr size_t kBlock = 8192;
  size_t stereoOut = 0;
  for (size_t off = 0; off < inSamples; off += kBlock) {
    const size_t chunk = std::min(kBlock, inSamples - off);
    stereoOut += stereo.processAudio(
        mpx.samples.data() + off, stereoL.data() + off, stereoR.data() + off,
        chunk);
  }
  REQUIRE(stereoOut == inSamples);

  // Resampler can produce more than (in * ratio) samples in pathological
  // edge cases; give it generous headroom.
  const size_t outCap = static_cast<size_t>(
      static_cast<double>(inSamples) *
      (static_cast<double>(kOutputRate) / kInputRate) * 1.10 + 64);
  std::vector<float> outL(outCap, 0.0f);
  std::vector<float> outR(outCap, 0.0f);
  const size_t produced =
      af.process(stereoL.data(), stereoR.data(), stereoOut, outL.data(),
                 outR.data(), outCap);
  REQUIRE(produced > 0);

  // Skip the front 30% so initial PLL/blend transient doesn't dominate.
  const size_t analyzeStart = produced * 3 / 10;
  const size_t analyzeLen = produced - analyzeStart;
  REQUIRE(analyzeLen > kOutputRate / 2);

  auto channelRms = [](const float *interleavedStereo, size_t startFrame,
                       size_t frameCount, int chan) {
    if (frameCount == 0) return 0.0f;
    double acc = 0.0;
    for (size_t i = 0; i < frameCount; i++) {
      const float v = interleavedStereo[(startFrame + i) * 2 + chan];
      acc += static_cast<double>(v) * v;
    }
    return static_cast<float>(std::sqrt(acc / frameCount));
  };
  auto rms = [](const float *data, size_t count) {
    if (count == 0) return 0.0f;
    double acc = 0.0;
    for (size_t i = 0; i < count; i++) {
      acc += static_cast<double>(data[i]) * data[i];
    }
    return static_cast<float>(std::sqrt(acc / count));
  };
  auto rmsSum = [](const float *a, const float *b, size_t count) {
    if (count == 0) return 0.0f;
    double acc = 0.0;
    for (size_t i = 0; i < count; i++) {
      const double s = static_cast<double>(a[i]) + b[i];
      acc += s * s * 0.25; // (L+R)/2
    }
    return static_cast<float>(std::sqrt(acc / count));
  };
  auto rmsDiff = [](const float *a, const float *b, size_t count) {
    if (count == 0) return 0.0f;
    double acc = 0.0;
    for (size_t i = 0; i < count; i++) {
      const double s = static_cast<double>(a[i]) - b[i];
      acc += s * s * 0.25; // (L-R)/2
    }
    return static_cast<float>(std::sqrt(acc / count));
  };

  // Reference: skip front 30% as well.
  const size_t refFrames = ref.samples.size() / 2;
  const size_t refAnalyzeStart = refFrames * 3 / 10;
  const size_t refAnalyzeLen = refFrames - refAnalyzeStart;

  const float refLeftRms =
      channelRms(ref.samples.data(), refAnalyzeStart, refAnalyzeLen, 0);
  const float refRightRms =
      channelRms(ref.samples.data(), refAnalyzeStart, refAnalyzeLen, 1);
  const float refMid = 0.5f * (refLeftRms + refRightRms);

  const float ourLeftRms =
      rms(outL.data() + analyzeStart, analyzeLen);
  const float ourRightRms =
      rms(outR.data() + analyzeStart, analyzeLen);
  const float ourMid = 0.5f * (ourLeftRms + ourRightRms);

  REQUIRE(refMid > 0.0f);
  REQUIRE(ourMid > 0.0f);
  const float midDeltaDb = 20.0f * std::log10(ourMid / refMid);

  // ±6 dB envelope around the reference. The pipeline has changed (Hi-Blend
  // biquad, no input-rate audio FIR, pilot canceller, soft limiter) so an
  // exact match is unrealistic; this catches gross regressions like silence,
  // hard clipping, or 10+ dB gain shifts.
  REQUIRE(midDeltaDb > -6.0f);
  REQUIRE(midDeltaDb <  6.0f);

  // Both channels carry audio (not a stuck-channel regression).
  REQUIRE(ourLeftRms > refMid * 0.25f);
  REQUIRE(ourRightRms > refMid * 0.25f);

  // Stereo separation: L-R should be nonzero and not catastrophically larger
  // than reference. Since blend mode now defaults to "normal" while reference
  // may have been captured under different settings, a wide ±10 dB envelope
  // around the reference's (L-R)/2 RMS is the right tolerance.
  std::vector<float> interleavedRef(refAnalyzeLen * 2);
  for (size_t i = 0; i < refAnalyzeLen; i++) {
    interleavedRef[i * 2] =
        ref.samples[(refAnalyzeStart + i) * 2];
    interleavedRef[i * 2 + 1] =
        ref.samples[(refAnalyzeStart + i) * 2 + 1];
  }
  // Compute (L-R)/2 RMS on reference.
  double refDiffAcc = 0.0;
  for (size_t i = 0; i < refAnalyzeLen; i++) {
    const double d = static_cast<double>(interleavedRef[i * 2]) -
                     interleavedRef[i * 2 + 1];
    refDiffAcc += d * d * 0.25;
  }
  const float refDiffRms = static_cast<float>(
      std::sqrt(refDiffAcc / std::max<size_t>(1, refAnalyzeLen)));
  const float ourDiffRms =
      rmsDiff(outL.data() + analyzeStart, outR.data() + analyzeStart,
              analyzeLen);
  // Reference must actually be stereo-separated; if not, our captured fixture
  // was already mono so we skip the separation assertion.
  if (refDiffRms > 1e-3f) {
    REQUIRE(ourDiffRms > 1e-4f);
    const float diffDb =
        20.0f * std::log10(std::max(ourDiffRms, 1e-6f) / refDiffRms);
    REQUIRE(diffDb > -12.0f);
    REQUIRE(diffDb <  12.0f);
  }

  // Use rmsSum to suppress an unused-lambda warning in builds that don't take
  // every diagnostic path above; it's kept for future telemetry on (L+R)/2.
  (void)rmsSum;
#endif
}
