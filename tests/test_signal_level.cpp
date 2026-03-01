#include "catch_compat.h"
#include <cstring>
#include "signal_level.h"

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
