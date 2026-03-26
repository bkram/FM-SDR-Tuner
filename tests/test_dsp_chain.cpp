#include "catch_compat.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <limits>
#include <vector>

#include "af_post_processor.h"
#include "config.h"
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
