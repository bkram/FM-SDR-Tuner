#include "catch_compat.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

#include "dsp/liquid_primitives.h"

// The scan keeps one spectrum object for the whole thread and re-uses it
// across retunes, so the test has to see the plan and the buffers.
#define private public
#include "signal_level.h"
#undef private

namespace {

std::vector<uint8_t> makeIqBuffer(size_t samples,
                                  uint32_t sampleRateHz,
                                  const std::vector<std::pair<float, float>>& tones,
                                  std::complex<float> dcOffset = {0.0f, 0.0f}) {
    std::vector<uint8_t> iq(samples * 2, 127);
    for (size_t n = 0; n < samples; ++n) {
        std::complex<float> sample = dcOffset;
        for (const auto& [freqHz, amplitude] : tones) {
            const float phase = 2.0f * 3.14159265358979323846f * freqHz *
                                static_cast<float>(n) /
                                static_cast<float>(sampleRateHz);
            sample += std::polar(amplitude, phase);
        }
        const float i = std::clamp(sample.real(), -0.999f, 0.999f);
        const float q = std::clamp(sample.imag(), -0.999f, 0.999f);
        iq[n * 2] = static_cast<uint8_t>(std::lround((i * 127.5f) + 127.5f));
        iq[n * 2 + 1] = static_cast<uint8_t>(std::lround((q * 127.5f) + 127.5f));
    }
    return iq;
}

std::vector<uint8_t> makeNoiseIqBuffer(size_t samples, float amplitude) {
    std::vector<uint8_t> iq(samples * 2, 127);
    uint32_t state = 0x13579bdfu;
    auto nextUnit = [&]() -> float {
        state = state * 1664525u + 1013904223u;
        const uint32_t bits = (state >> 8) & 0x00ffffffu;
        return (static_cast<float>(bits) / 8388607.5f) - 1.0f;
    };
    for (size_t n = 0; n < samples; ++n) {
        const float i = std::clamp(nextUnit() * amplitude, -0.999f, 0.999f);
        const float q = std::clamp(nextUnit() * amplitude, -0.999f, 0.999f);
        iq[n * 2] = static_cast<uint8_t>(std::lround((i * 127.5f) + 127.5f));
        iq[n * 2 + 1] = static_cast<uint8_t>(std::lround((q * 127.5f) + 127.5f));
    }
    return iq;
}

// The meter tracks the tuner's DC offset across blocks with a 0.5 s time
// constant. Feed it ~4 s of `iq` so it reads in its steady state.
void settleMeterDcTracking(const std::vector<uint8_t>& iq, size_t samples,
                           uint32_t sampleRateHz) {
    resetSignalLevelDcTracking();
    const size_t blocks = (4 * sampleRateHz) / samples + 1;
    for (size_t b = 0; b < blocks; ++b) {
        (void)computeSignalLevel(iq.data(), samples, 0, 0.5, 0.0, -80.0, -12.0,
                                 sampleRateHz);
    }
}

} // namespace

TEST_CASE("computeSignalLevel handles null input", "[signal_level]") {
    SignalLevelResult result = computeSignalLevel(nullptr, 100, 0, 0.5, 0.0, -80.0, -12.0);
    REQUIRE(result.dbfs == -120.0);
    REQUIRE(result.level120 == 0.0f);
}

TEST_CASE("computeSignalLevel handles zero samples", "[signal_level]") {
    uint8_t buffer[256];
    SignalLevelResult result = computeSignalLevel(buffer, 0, 0, 0.5, 0.0, -80.0, -12.0);
    REQUIRE(result.dbfs == -120.0);
    REQUIRE(result.level120 == 0.0f);
}

TEST_CASE("computeSignalLevel computes level for silent input", "[signal_level]") {
    uint8_t buffer[256];
    std::memset(buffer, 127, sizeof(buffer));
    
    SignalLevelResult result = computeSignalLevel(buffer, 128, 0, 0.5, 0.0, -80.0, -12.0);
    REQUIRE(result.dbfs < -60.0);
    REQUIRE(result.level120 == 0.0f);
}

TEST_CASE("smoothSignalLevel initializes on first call", "[signal_level]") {
    SignalLevelSmoother state;
    REQUIRE(state.initialized == false);
    
    float result = smoothSignalLevel(50.0f, state);
    REQUIRE(result == 50.0f);
    REQUIRE(state.initialized == true);
    REQUIRE(state.value == 50.0f);
}

TEST_CASE("smoothSignalLevel smooths subsequent values", "[signal_level]") {
    SignalLevelSmoother state;
    state.initialized = true;
    state.value = 50.0f;
    
    float result = smoothSignalLevel(60.0f, state);
    REQUIRE(result > 50.0f);
    REQUIRE(result < 60.0f);
}

TEST_CASE("computeSignalLevel reports clip ratio", "[signal_level]") {
    uint8_t buffer[256];
    std::memset(buffer, 0, sizeof(buffer));
    
    SignalLevelResult result = computeSignalLevel(buffer, 128, 0, 0.5, 0.0, -80.0, -12.0);
    REQUIRE(result.hardClipRatio > 0.0);
}

TEST_CASE("computeSignalLevel near clip ratio is not below hard clip ratio", "[signal_level]") {
    uint8_t buffer[256];
    std::memset(buffer, 255, sizeof(buffer));

    const SignalLevelResult result = computeSignalLevel(buffer, 128, 0, 0.5, 0.0, -80.0, -12.0);
    REQUIRE(result.nearClipRatio >= result.hardClipRatio);
    REQUIRE(result.hardClipRatio > 0.9);
}

TEST_CASE("computeSignalLevel compensation decreases with applied gain", "[signal_level]") {
    uint8_t buffer[256];
    for (size_t i = 0; i < sizeof(buffer); i += 2) {
        buffer[i] = 200;
        buffer[i + 1] = 80;
    }

    const SignalLevelResult noGain = computeSignalLevel(buffer, 128, 0, 0.5, 0.0, -80.0, -12.0);
    const SignalLevelResult highGain = computeSignalLevel(buffer, 128, 40, 0.5, 0.0, -80.0, -12.0);

    REQUIRE(highGain.compensatedDbfs < noGain.compensatedDbfs);
}

TEST_CASE("computeSignalLevel level is clamped to [0,120]", "[signal_level]") {
    uint8_t loud[256];
    std::memset(loud, 255, sizeof(loud));
    const SignalLevelResult high = computeSignalLevel(loud, 128, 0, 0.5, 0.0, -80.0, -12.0);
    REQUIRE(high.level120 >= 0.0f);
    REQUIRE(high.level120 <= 120.0f);

    uint8_t quiet[256];
    std::memset(quiet, 127, sizeof(quiet));
    const SignalLevelResult low = computeSignalLevel(quiet, 128, 40, 1.0, -30.0, -80.0, -12.0);
    REQUIRE(low.level120 == 0.0f);
}

TEST_CASE("smoothSignalLevel rises faster than it falls", "[signal_level]") {
    SignalLevelSmoother state;
    state.initialized = true;
    state.value = 50.0f;

    const float rise = smoothSignalLevel(60.0f, state);
    const float afterRise = state.value;
    const float fall = smoothSignalLevel(50.0f, state);

    REQUIRE((rise - 50.0f) > (afterRise - fall));
}

TEST_CASE("computeSignalLevel tracks stronger IQ amplitude with higher dbfs", "[signal_level]") {
    uint8_t weak[256];
    uint8_t strong[256];
    for (size_t i = 0; i < 256; i += 2) {
        if (((i / 2) % 2) == 0) {
            weak[i] = 132;
            weak[i + 1] = 123;
            strong[i] = 220;
            strong[i + 1] = 34;
        } else {
            weak[i] = 123;
            weak[i + 1] = 132;
            strong[i] = 34;
            strong[i + 1] = 220;
        }
    }

    const SignalLevelResult weakResult = computeSignalLevel(weak, 128, 0, 0.5, 0.0, -80.0, -12.0);
    const SignalLevelResult strongResult = computeSignalLevel(strong, 128, 0, 0.5, 0.0, -80.0, -12.0);

    REQUIRE(strongResult.dbfs > weakResult.dbfs);
    REQUIRE(strongResult.level120 >= weakResult.level120);
}

TEST_CASE("computeSignalLevel prefers in-channel power over strong out-of-channel blocker",
          "[signal_level]") {
    constexpr size_t kSamples = 4096;
    constexpr uint32_t kSampleRateHz = 256000;

    // The measurement window is 30 kHz wide, so the in-channel tone has to
    // sit well inside it and the blocker well outside it.
    const std::vector<uint8_t> inChannel =
        makeIqBuffer(kSamples, kSampleRateHz, {{8000.0f, 0.18f}});
    const std::vector<uint8_t> outOfChannel =
        makeIqBuffer(kSamples, kSampleRateHz, {{120000.0f, 0.70f}});
    const std::vector<uint8_t> mixed =
        makeIqBuffer(kSamples, kSampleRateHz,
                     {{8000.0f, 0.18f}, {120000.0f, 0.70f}});

    const SignalLevelResult inChannelLevel =
        computeSignalLevel(inChannel.data(), kSamples, 0, 0.5, 0.0, -80.0, -12.0,
                           kSampleRateHz);
    const SignalLevelResult outOfChannelLevel =
        computeSignalLevel(outOfChannel.data(), kSamples, 0, 0.5, 0.0, -80.0,
                           -12.0, kSampleRateHz);
    const SignalLevelResult mixedLevel =
        computeSignalLevel(mixed.data(), kSamples, 0, 0.5, 0.0, -80.0, -12.0,
                           kSampleRateHz);

    REQUIRE(inChannelLevel.dbfs > outOfChannelLevel.dbfs + 8.0);
    REQUIRE(std::abs(mixedLevel.dbfs - inChannelLevel.dbfs) < 2.5);
    REQUIRE(mixedLevel.snrDb > 0.0);
}

TEST_CASE("computeSignalLevel uses the shared measurement window when the sample rate is known",
          "[signal_level]") {
    constexpr size_t kSamples = 4096;
    constexpr uint32_t kSampleRateHz = 256000;
    const std::vector<uint8_t> iq =
        makeIqBuffer(kSamples, kSampleRateHz, {{12000.0f, 0.14f}, {120000.0f, 0.55f}});

    const SignalLevelResult wideband =
        computeSignalLevel(iq.data(), kSamples, 0, 0.5, 0.0, -80.0, -12.0);
    const SignalLevelResult channelAware =
        computeSignalLevel(iq.data(), kSamples, 0, 0.5, 0.0, -80.0, -12.0,
                           kSampleRateHz);

    REQUIRE(channelAware.dbfs < wideband.dbfs - 4.0);
    REQUIRE(channelAware.noiseFloorDbfs <= channelAware.dbfs);
}

TEST_CASE("computeSignalLevel reads a noise-only channel low and a station high",
          "[signal_level]") {
    constexpr size_t kSamples = 4096;
    constexpr uint32_t kSampleRateHz = 256000;
    // The production calibration window: the meter integrates the whole
    // 200 kHz channel, where an empty band reads about -47 dBFS (8 dB above
    // the same noise in the scan's 30 kHz window) and a strong local station
    // about -15 dBFS, so the noise has to read at the bottom of the scale and
    // the station at the top.
    constexpr double kFloorDbfs = -52.0;
    constexpr double kCeilDbfs = -20.0;

    const std::vector<uint8_t> floorOnly =
        makeNoiseIqBuffer(kSamples, 0.002f);
    const std::vector<uint8_t> strongStation =
        makeIqBuffer(kSamples, kSampleRateHz, {{8000.0f, 0.18f}});

    settleMeterDcTracking(floorOnly, kSamples, kSampleRateHz);
    const SignalLevelResult floorOnlyLevel =
        computeSignalLevel(floorOnly.data(), kSamples, 0, 0.5, 0.0, kFloorDbfs,
                           kCeilDbfs, kSampleRateHz);
    resetSignalLevelDcTracking();
    const SignalLevelResult strongStationLevel =
        computeSignalLevel(strongStation.data(), kSamples, 0, 0.5, 0.0,
                           kFloorDbfs, kCeilDbfs, kSampleRateHz);

    REQUIRE(floorOnlyLevel.snrDb < 6.0);
    REQUIRE(floorOnlyLevel.level120 < 25.0f);
    REQUIRE(strongStationLevel.level120 > 90.0f);
    REQUIRE(strongStationLevel.level120 > floorOnlyLevel.level120 + 60.0f);
}

TEST_CASE("computeDisplaySignalLevel120 absolute receiver path is not gated by unrelated floor",
          "[signal_level]") {
    const float receiverLevel = computeDisplaySignalLevel120(
        -45.0, -20.0, 0, 0.5, 0.0, -80.0, -12.0, false);
    const float gatedLevel = computeDisplaySignalLevel120(
        -45.0, -20.0, 0, 0.5, 0.0, -80.0, -12.0, true);

    REQUIRE(receiverLevel > 40.0f);
    REQUIRE(gatedLevel == 0.0f);
}

TEST_CASE("snrLevel120FromSnrDb suppresses noise, climbs with SNR, uncaps on NaN",
          "[signal_level]") {
    // The display path caps its (saturation-prone) absolute level by this
    // SNR-derived level so empty spectrum reads low and only genuine signal
    // climbs the meter. Below/at the gate -> 0; NaN -> no cap (120).
    REQUIRE(snrLevel120FromSnrDb(0.0) == 0.0f);
    REQUIRE(snrLevel120FromSnrDb(3.0) == 0.0f);
    const float mid = snrLevel120FromSnrDb(16.0);
    REQUIRE(mid > 40.0f);
    REQUIRE(mid < 90.0f);
    REQUIRE(snrLevel120FromSnrDb(30.0) == 120.0f);
    REQUIRE(snrLevel120FromSnrDb(60.0) == 120.0f);
    REQUIRE(snrLevel120FromSnrDb(
                std::numeric_limits<double>::quiet_NaN()) == 120.0f);

    // An empty channel must never read higher than a strong one.
    REQUIRE(snrLevel120FromSnrDb(2.5) < snrLevel120FromSnrDb(22.0));
}

TEST_CASE("computeSignalLevel rejects center DC contamination", "[signal_level]") {
    constexpr size_t kSamples = 4096;
    constexpr uint32_t kSampleRateHz = 256000;

    const std::vector<uint8_t> dcOnly =
        makeIqBuffer(kSamples, kSampleRateHz, {}, {0.55f, 0.0f});
    const std::vector<uint8_t> toneWithDc =
        makeIqBuffer(kSamples, kSampleRateHz, {{8000.0f, 0.12f}}, {0.55f, 0.0f});
    const std::vector<uint8_t> toneOnly =
        makeIqBuffer(kSamples, kSampleRateHz, {{8000.0f, 0.12f}});

    // The tuner's DC offset is static, so the meter's slow DC tracking
    // removes it once settled.
    settleMeterDcTracking(dcOnly, kSamples, kSampleRateHz);
    const SignalLevelResult dcOnlyLevel =
        computeSignalLevel(dcOnly.data(), kSamples, 0, 0.5, 0.0, -80.0, -12.0,
                           kSampleRateHz);
    const SignalLevelResult toneWithDcLevel =
        computeSignalLevel(toneWithDc.data(), kSamples, 0, 0.5, 0.0, -80.0,
                           -12.0, kSampleRateHz);
    resetSignalLevelDcTracking();
    const SignalLevelResult toneOnlyLevel =
        computeSignalLevel(toneOnly.data(), kSamples, 0, 0.5, 0.0, -80.0, -12.0,
                           kSampleRateHz);

    REQUIRE(dcOnlyLevel.dbfs < -45.0);
    REQUIRE(std::abs(toneWithDcLevel.dbfs - toneOnlyLevel.dbfs) < 2.0);
}

TEST_CASE("computeSignalLevel remains selective at higher sample rates",
          "[signal_level]") {
    constexpr size_t kSamples = 8192;
    constexpr uint32_t kSampleRateHz = 1024000;

    const std::vector<uint8_t> inChannel =
        makeIqBuffer(kSamples, kSampleRateHz, {{12000.0f, 0.18f}});
    const std::vector<uint8_t> blocker =
        makeIqBuffer(kSamples, kSampleRateHz, {{320000.0f, 0.70f}});
    const std::vector<uint8_t> mixed =
        makeIqBuffer(kSamples, kSampleRateHz,
                     {{12000.0f, 0.18f}, {320000.0f, 0.70f}});

    const SignalLevelResult inChannelLevel =
        computeSignalLevel(inChannel.data(), kSamples, 0, 0.5, 0.0, -80.0, -12.0,
                           kSampleRateHz);
    const SignalLevelResult blockerLevel =
        computeSignalLevel(blocker.data(), kSamples, 0, 0.5, 0.0, -80.0, -12.0,
                           kSampleRateHz);
    const SignalLevelResult mixedLevel =
        computeSignalLevel(mixed.data(), kSamples, 0, 0.5, 0.0, -80.0, -12.0,
                           kSampleRateHz);

    REQUIRE(inChannelLevel.dbfs > blockerLevel.dbfs + 8.0);
    REQUIRE(std::abs(mixedLevel.dbfs - inChannelLevel.dbfs) < 2.5);
}

TEST_CASE("signal level meter stays sane for every block size and sample rate",
          "[signal_level]") {
  // The measurement window is 30 kHz wide, so it can be wider than the
  // analysed spectrum: the band is then clipped to the spectrum instead of
  // running off the end of the FFT output (which used to read past the end
  // of the array and wrap the noise window back over the signal).
  for (uint32_t sampleRateHz : {256000u, 512000u, 1024000u}) {
    for (size_t samples : {size_t{1024}, size_t{2048}, size_t{4096},
                           size_t{8192}, size_t{16384}}) {
      const std::vector<uint8_t> iq =
          makeIqBuffer(samples, sampleRateHz, {{10000.0f, 0.5f}});
      const SignalLevelResult r = computeSignalLevel(
          iq.data(), samples, 0, 0.5, 0.0, -65.0, -5.0, sampleRateHz);
      REQUIRE(std::isfinite(r.dbfs));
      REQUIRE(std::isfinite(r.noiseFloorDbfs));
      REQUIRE(std::isfinite(r.level120));
      REQUIRE(r.level120 >= 0.0f);
      REQUIRE(r.level120 <= 120.0f);
      // A station inside the window has to read as a station.
      REQUIRE(r.dbfs > -60.0);
      REQUIRE(r.level120 > 0.0f);
    }
  }
}

TEST_CASE("clip ratio counts bytes, not samples", "[signal_level]") {
  // The clip ratio is a fraction of IQ *bytes* (the denominator is
  // samples * 2). Counting a whole sample whenever either byte was
  // saturated reported twice the true fraction, so a station whose I
  // component alone clipped looked 2x over the auto-gain step-down
  // threshold and lost several dB of sensitivity for no reason.
  constexpr size_t kSamples = 1000;
  std::vector<uint8_t> iq(kSamples * 2, 128);
  // Only the I byte is saturated.
  for (size_t s = 0; s < kSamples; s++) {
    iq[2 * s] = 255;
  }
  const SignalLevelResult r =
      computeSignalLevel(iq.data(), kSamples, 0, 0.5, 0.0, -65.0, -5.0);
  REQUIRE(r.hardClipRatio > 0.49);
  REQUIRE(r.hardClipRatio < 0.51);

  // Both bytes saturated: the ratio is then 1.0.
  for (size_t s = 0; s < kSamples; s++) {
    iq[2 * s + 1] = 255;
  }
  const SignalLevelResult r2 =
      computeSignalLevel(iq.data(), kSamples, 0, 0.5, 0.0, -65.0, -5.0);
  REQUIRE(r2.hardClipRatio > 0.99);
}

TEST_CASE("overload ghosting is only suspected on a clipping strong channel",
          "[signal_level]") {
  SignalLevelResult strong;
  strong.level120 = 95.0f;
  strong.dbfs = -8.0;
  strong.hardClipRatio = 0.05;
  strong.nearClipRatio = 0.0;

  // All three conditions hold: clipping, a strong channel, a loud station.
  REQUIRE(suspectOverloadGhosting(strong, 0.05));

  // No clipping: a loud but clean station cannot produce an overload image.
  REQUIRE_FALSE(suspectOverloadGhosting(strong, 0.0));

  // Clipping, but a weak channel: the front end is not being driven by a
  // strong station, so this is not the overload image.
  SignalLevelResult weak = strong;
  weak.level120 = 20.0f;
  weak.dbfs = -50.0;
  REQUIRE_FALSE(suspectOverloadGhosting(weak, 0.05));

  // Clipping and a strong channel, but the channel is not actually loud.
  SignalLevelResult quiet = strong;
  quiet.dbfs = -45.0;
  REQUIRE_FALSE(suspectOverloadGhosting(quiet, 0.05));
}

TEST_CASE("the scan spectrum re-uses its FFT plan and buffers across retunes",
          "[signal_level][spectrum]") {
    // The scan engine holds one spectrum object for the whole thread and
    // re-uses it for every retune: the plan and the buffers have to survive
    // (re-creating the plan per block is what the scan's CPU budget cannot
    // afford) and the result must not carry the previous block over.
    constexpr uint32_t kRate = 256000;
    constexpr size_t kSamples = 8192;
    constexpr int kWindowHz = 30000;

    const std::vector<uint8_t> highTone =
        makeIqBuffer(kSamples, kRate, {{50000.0f, 0.5f}});
    const std::vector<uint8_t> lowTone =
        makeIqBuffer(kSamples, kRate, {{-50000.0f, 0.5f}});
    const std::vector<uint8_t> highToneLong =
        makeIqBuffer(kSamples * 2, kRate, {{50000.0f, 0.5f}});
    const std::vector<uint8_t> lowToneLong =
        makeIqBuffer(kSamples * 2, kRate, {{-50000.0f, 0.5f}});

    IqSpectrum spectrum;
    REQUIRE(spectrum.analyse(highTone.data(), kSamples, kRate));
    REQUIRE(spectrum.valid());
    const std::complex<float> *fftIn = spectrum.m_fftIn.data();
    const std::complex<float> *fftOut = spectrum.m_fftOut.data();
    const size_t nfft = spectrum.nfft();
    REQUIRE(spectrum.planCreations() == 1);
    REQUIRE(nfft == kSamples);
    REQUIRE(spectrum.channelDbfs(50000, kWindowHz) >
            spectrum.channelDbfs(-50000, kWindowHz) + 20.0);

    // A retune: the same block size, new content.
    REQUIRE(spectrum.analyse(lowTone.data(), kSamples, kRate));
    REQUIRE(spectrum.planCreations() == 1);     // the plan is re-used
    REQUIRE(spectrum.m_fftIn.data() == fftIn);  // ... and so are the buffers
    REQUIRE(spectrum.m_fftOut.data() == fftOut);
    REQUIRE(spectrum.nfft() == nfft);

    // The new block's spectrum is the new block's: the tone moved.
    REQUIRE(spectrum.channelDbfs(-50000, kWindowHz) >
            spectrum.channelDbfs(50000, kWindowHz) + 20.0);

    // Re-analysing the same block gives the same numbers (the buffers are
    // filled completely every time, not appended to).
    const double first = spectrum.channelDbfs(-50000, kWindowHz);
    REQUIRE(spectrum.analyse(lowTone.data(), kSamples, kRate));
    REQUIRE(std::abs(spectrum.channelDbfs(-50000, kWindowHz) - first) < 1e-9);

    // A different block size re-creates the plan, and the result is still
    // the new block's.
    REQUIRE(spectrum.analyse(highToneLong.data(), kSamples * 2, kRate));
    REQUIRE(spectrum.planCreations() == 2);  // a new size needs a new plan
    REQUIRE(spectrum.nfft() == kSamples * 2);
    REQUIRE(spectrum.channelDbfs(50000, kWindowHz) >
            spectrum.channelDbfs(-50000, kWindowHz) + 20.0);

    // ... and back to the smaller block, which needs a new plan again.
    REQUIRE(spectrum.analyse(lowTone.data(), kSamples, kRate));
    REQUIRE(spectrum.planCreations() == 3);
    REQUIRE(spectrum.nfft() == kSamples);
    REQUIRE(spectrum.channelDbfs(-50000, kWindowHz) >
            spectrum.channelDbfs(50000, kWindowHz) + 20.0);
}

namespace {

// A continuous FM carrier at `offsetHz` from the tuned frequency, modulated
// by a `toneHz` tone at `deviationHz` peak deviation, quantised to uint8 IQ.
// Consecutive calls continue the same signal, as consecutive tuner blocks do.
class FmCarrier {
public:
    FmCarrier(uint32_t sampleRateHz, double offsetHz, double amplitude,
              double deviationHz = 0.0, double toneHz = 3000.0)
        : m_fs(sampleRateHz), m_offset(offsetHz), m_amp(amplitude),
          m_dev(deviationHz), m_tone(toneHz) {}

    std::vector<uint8_t> next(size_t samples) {
        constexpr double kTwoPi = 6.283185307179586;
        std::vector<uint8_t> iq(samples * 2);
        for (size_t i = 0; i < samples; ++i) {
            const double t = static_cast<double>(m_n++) / m_fs;
            const double phase =
                kTwoPi * m_offset * t +
                (m_dev / m_tone) * std::sin(kTwoPi * m_tone * t);
            const double re = m_amp * std::cos(phase);
            const double im = m_amp * std::sin(phase);
            iq[i * 2] = static_cast<uint8_t>(std::lround(re * 127.5 + 127.5));
            iq[i * 2 + 1] = static_cast<uint8_t>(std::lround(im * 127.5 + 127.5));
        }
        return iq;
    }

private:
    double m_fs;
    double m_offset;
    double m_amp;
    double m_dev;
    double m_tone;
    uint64_t m_n = 0;
};

double meterDbfsAfter(FmCarrier& carrier, double seconds, uint32_t fs) {
    constexpr size_t kBlock = 8192;
    const size_t blocks = static_cast<size_t>(seconds * fs) / kBlock;
    double dbfs = -200.0;
    for (size_t b = 0; b < blocks; ++b) {
        const std::vector<uint8_t> iq = carrier.next(kBlock);
        dbfs = computeSignalLevel(iq.data(), kBlock, 0, 0.5, 0.0, -80.0, -12.0,
                                  fs).dbfs;
    }
    return dbfs;
}

} // namespace

TEST_CASE("the tuned meter does not blank a carrier a few Hz off tune",
          "[signal_level]") {
    // A quiet passage on a station tuned to within a few Hz: the carrier is
    // nearly constant over one block. Subtracting each block's mean removed
    // the carrier itself and the meter read -27 dBFS instead of -10.5.
    constexpr uint32_t kFs = 256000;
    const double expectedDbfs = 20.0 * std::log10(0.3);
    for (double offsetHz : {5.0, 15.0, 2000.0}) {
        resetSignalLevelDcTracking();
        FmCarrier carrier(kFs, offsetHz, 0.3);
        const double dbfs = meterDbfsAfter(carrier, 4.0, kFs);
        INFO("offset " << offsetHz << " Hz: " << dbfs << " dBFS, expected "
                       << expectedDbfs);
        REQUIRE(std::abs(dbfs - expectedDbfs) < 1.0);
    }
    resetSignalLevelDcTracking();
}

TEST_CASE("the tuned meter does not pump with the programme",
          "[signal_level]") {
    // An FM signal has a constant envelope: the same carrier fully modulated
    // must read what it reads in silence. With the 30 kHz scan window the
    // meter fell from -10.7 to -19.0 dBFS at 75 kHz deviation.
    constexpr uint32_t kFs = 256000;
    resetSignalLevelDcTracking();
    FmCarrier silent(kFs, 2000.0, 0.3);
    const double silentDbfs = meterDbfsAfter(silent, 1.0, kFs);
    resetSignalLevelDcTracking();
    FmCarrier loud(kFs, 2000.0, 0.3, 75000.0, 3000.0);
    const double loudDbfs = meterDbfsAfter(loud, 1.0, kFs);
    resetSignalLevelDcTracking();
    INFO("silent " << silentDbfs << " dBFS, 75 kHz deviation " << loudDbfs
                   << " dBFS");
    REQUIRE(std::abs(silentDbfs - loudDbfs) < 1.0);
}
