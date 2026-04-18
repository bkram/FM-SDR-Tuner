#include "catch_compat.h"
#include "signal_level.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>
#include <vector>

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
    constexpr int kChannelBandwidthHz = 56000;

    const std::vector<uint8_t> inChannel =
        makeIqBuffer(kSamples, kSampleRateHz, {{15000.0f, 0.18f}});
    const std::vector<uint8_t> outOfChannel =
        makeIqBuffer(kSamples, kSampleRateHz, {{90000.0f, 0.70f}});
    const std::vector<uint8_t> mixed =
        makeIqBuffer(kSamples, kSampleRateHz,
                     {{15000.0f, 0.18f}, {90000.0f, 0.70f}});

    const SignalLevelResult inChannelLevel =
        computeSignalLevel(inChannel.data(), kSamples, 0, 0.5, 0.0, -80.0, -12.0,
                           kSampleRateHz, kChannelBandwidthHz);
    const SignalLevelResult outOfChannelLevel =
        computeSignalLevel(outOfChannel.data(), kSamples, 0, 0.5, 0.0, -80.0,
                           -12.0, kSampleRateHz, kChannelBandwidthHz);
    const SignalLevelResult mixedLevel =
        computeSignalLevel(mixed.data(), kSamples, 0, 0.5, 0.0, -80.0, -12.0,
                           kSampleRateHz, kChannelBandwidthHz);

    REQUIRE(inChannelLevel.dbfs > outOfChannelLevel.dbfs + 8.0);
    REQUIRE(std::abs(mixedLevel.dbfs - inChannelLevel.dbfs) < 2.5);
    REQUIRE(mixedLevel.snrDb > 0.0);
}

TEST_CASE("computeSignalLevel uses channel bandwidth aware meter when sample rate is known",
          "[signal_level]") {
    constexpr size_t kSamples = 4096;
    constexpr uint32_t kSampleRateHz = 256000;
    const std::vector<uint8_t> iq =
        makeIqBuffer(kSamples, kSampleRateHz, {{12000.0f, 0.14f}, {85000.0f, 0.55f}});

    const SignalLevelResult wideband =
        computeSignalLevel(iq.data(), kSamples, 0, 0.5, 0.0, -80.0, -12.0);
    const SignalLevelResult channelAware =
        computeSignalLevel(iq.data(), kSamples, 0, 0.5, 0.0, -80.0, -12.0,
                           kSampleRateHz, 56000);

    REQUIRE(channelAware.dbfs < wideband.dbfs - 4.0);
    REQUIRE(channelAware.noiseFloorDbfs <= channelAware.dbfs);
}

TEST_CASE("computeSignalLevel suppresses noise-only channels in displayed level",
          "[signal_level]") {
    constexpr size_t kSamples = 4096;
    constexpr uint32_t kSampleRateHz = 256000;
    constexpr int kChannelBandwidthHz = 56000;

    const std::vector<uint8_t> floorOnly =
        makeNoiseIqBuffer(kSamples, 0.08f);
    const std::vector<uint8_t> strongStation =
        makeIqBuffer(kSamples, kSampleRateHz, {{12000.0f, 0.18f}});

    const SignalLevelResult floorOnlyLevel =
        computeSignalLevel(floorOnly.data(), kSamples, 0, 0.5, 0.0, -80.0,
                           -12.0, kSampleRateHz, kChannelBandwidthHz);
    const SignalLevelResult strongStationLevel =
        computeSignalLevel(strongStation.data(), kSamples, 0, 0.5, 0.0, -80.0,
                           -12.0, kSampleRateHz, kChannelBandwidthHz);

    REQUIRE(floorOnlyLevel.snrDb < 6.0);
    REQUIRE(floorOnlyLevel.level120 < 12.0f);
    REQUIRE(strongStationLevel.level120 > floorOnlyLevel.level120 + 20.0f);
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

TEST_CASE("computeSignalLevel rejects center DC contamination", "[signal_level]") {
    constexpr size_t kSamples = 4096;
    constexpr uint32_t kSampleRateHz = 256000;
    constexpr int kChannelBandwidthHz = 56000;

    const std::vector<uint8_t> dcOnly =
        makeIqBuffer(kSamples, kSampleRateHz, {}, {0.55f, 0.0f});
    const std::vector<uint8_t> toneWithDc =
        makeIqBuffer(kSamples, kSampleRateHz, {{15000.0f, 0.12f}}, {0.55f, 0.0f});
    const std::vector<uint8_t> toneOnly =
        makeIqBuffer(kSamples, kSampleRateHz, {{15000.0f, 0.12f}});

    const SignalLevelResult dcOnlyLevel =
        computeSignalLevel(dcOnly.data(), kSamples, 0, 0.5, 0.0, -80.0, -12.0,
                           kSampleRateHz, kChannelBandwidthHz);
    const SignalLevelResult toneWithDcLevel =
        computeSignalLevel(toneWithDc.data(), kSamples, 0, 0.5, 0.0, -80.0,
                           -12.0, kSampleRateHz, kChannelBandwidthHz);
    const SignalLevelResult toneOnlyLevel =
        computeSignalLevel(toneOnly.data(), kSamples, 0, 0.5, 0.0, -80.0, -12.0,
                           kSampleRateHz, kChannelBandwidthHz);

    REQUIRE(dcOnlyLevel.dbfs < -45.0);
    REQUIRE(std::abs(toneWithDcLevel.dbfs - toneOnlyLevel.dbfs) < 2.0);
}

TEST_CASE("computeSignalLevel remains selective at higher sample rates",
          "[signal_level]") {
    constexpr size_t kSamples = 8192;
    constexpr uint32_t kSampleRateHz = 1024000;
    constexpr int kChannelBandwidthHz = 56000;

    const std::vector<uint8_t> inChannel =
        makeIqBuffer(kSamples, kSampleRateHz, {{12000.0f, 0.18f}});
    const std::vector<uint8_t> blocker =
        makeIqBuffer(kSamples, kSampleRateHz, {{320000.0f, 0.70f}});
    const std::vector<uint8_t> mixed =
        makeIqBuffer(kSamples, kSampleRateHz,
                     {{12000.0f, 0.18f}, {320000.0f, 0.70f}});

    const SignalLevelResult inChannelLevel =
        computeSignalLevel(inChannel.data(), kSamples, 0, 0.5, 0.0, -80.0, -12.0,
                           kSampleRateHz, kChannelBandwidthHz);
    const SignalLevelResult blockerLevel =
        computeSignalLevel(blocker.data(), kSamples, 0, 0.5, 0.0, -80.0, -12.0,
                           kSampleRateHz, kChannelBandwidthHz);
    const SignalLevelResult mixedLevel =
        computeSignalLevel(mixed.data(), kSamples, 0, 0.5, 0.0, -80.0, -12.0,
                           kSampleRateHz, kChannelBandwidthHz);

    REQUIRE(inChannelLevel.dbfs > blockerLevel.dbfs + 8.0);
    REQUIRE(std::abs(mixedLevel.dbfs - inChannelLevel.dbfs) < 2.5);
}
