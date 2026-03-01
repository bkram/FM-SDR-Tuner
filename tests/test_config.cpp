#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include "config.h"

TEST_CASE("Config loads defaults", "[config]") {
    Config config;
    config.loadDefaults();
    
    REQUIRE(config.rtl_tcp.host == "localhost");
    REQUIRE(config.rtl_tcp.port == 1234);
    REQUIRE(config.audio.startup_volume == 100);
}

TEST_CASE("Config parses rtl_tcp section", "[config]") {
    Config config;
    config.loadDefaults();
    
    std::ofstream file("test_config.ini");
    file << "[rtl_tcp]\n";
    file << "host = 192.168.1.1\n";
    file << "port = 5678\n";
    file << "sample_rate = 1024000\n";
    file.close();
    
    bool result = config.loadFromFile("test_config.ini");
    REQUIRE(result == true);
    REQUIRE(config.rtl_tcp.host == "192.168.1.1");
    REQUIRE(config.rtl_tcp.port == 5678);
    REQUIRE(config.rtl_tcp.sample_rate == 1024000);
    
    std::remove("test_config.ini");
}

TEST_CASE("Config parses audio section", "[config]") {
    Config config;
    config.loadDefaults();
    
    std::ofstream file("test_config.ini");
    file << "[audio]\n";
    file << "enable_audio = true\n";
    file << "startup_volume = 50\n";
    file.close();
    
    bool result = config.loadFromFile("test_config.ini");
    REQUIRE(result == true);
    REQUIRE(config.audio.enable_audio == true);
    REQUIRE(config.audio.startup_volume == 50);
    
    std::remove("test_config.ini");
}

TEST_CASE("Config parses sdr section with gain strategy", "[config]") {
    Config config;
    config.loadDefaults();
    
    std::ofstream file("test_config.ini");
    file << "[sdr]\n";
    file << "gain_strategy = tef\n";
    file << "signal_bias_db = 5.0\n";
    file.close();
    
    bool result = config.loadFromFile("test_config.ini");
    REQUIRE(result == true);
    REQUIRE(config.sdr.gain_strategy == "tef");
    REQUIRE(config.sdr.signal_bias_db == 5.0);
    
    std::remove("test_config.ini");
}

TEST_CASE("Config handles invalid values gracefully", "[config]") {
    Config config;
    config.loadDefaults();
    
    std::ofstream file("test_config.ini");
    file << "[rtl_tcp]\n";
    file << "port = 99999\n";
    file << "sample_rate = 123\n";
    file.close();
    
    bool result = config.loadFromFile("test_config.ini");
    REQUIRE(result == true);
    REQUIRE(config.rtl_tcp.port == 1234);
    REQUIRE(config.rtl_tcp.sample_rate == 256000);
    
    std::remove("test_config.ini");
}

TEST_CASE("Config ignores unknown sections", "[config]") {
    Config config;
    config.loadDefaults();
    
    std::ofstream file("test_config.ini");
    file << "[unknown_section]\n";
    file << "foo = bar\n";
    file.close();
    
    bool result = config.loadFromFile("test_config.ini");
    REQUIRE(result == true);
    
    std::remove("test_config.ini");
}

TEST_CASE("Config clamps SDR numeric ranges", "[config]") {
    Config config;
    config.loadDefaults();

    std::ofstream file("test_config.ini");
    file << "[sdr]\n";
    file << "freq_correction_ppm = 999\n";
    file << "signal_bias_db = -99\n";
    file << "sdrpp_rtl_agc_gain_db = 99\n";
    file.close();

    const bool result = config.loadFromFile("test_config.ini");
    REQUIRE(result == true);
    REQUIRE(config.sdr.freq_correction_ppm == 250);
    REQUIRE(config.sdr.signal_bias_db == -30.0);
    REQUIRE(config.sdr.sdrpp_rtl_agc_gain_db == 28);

    std::remove("test_config.ini");
}

TEST_CASE("Config normalizes custom gain flags into RF IF bits", "[config]") {
    Config config;
    config.loadDefaults();

    std::ofstream file("test_config.ini");
    file << "[sdr]\n";
    file << "default_custom_gain_flags = 57\n";
    file.close();

    const bool result = config.loadFromFile("test_config.ini");
    REQUIRE(result == true);
    REQUIRE(config.sdr.default_custom_gain_flags == 11);

    std::remove("test_config.ini");
}

TEST_CASE("Config parses and clamps processing fields", "[config]") {
    Config config;
    config.loadDefaults();

    std::ofstream file("test_config.ini");
    file << "[processing]\n";
    file << "dsp_block_samples = 999999\n";
    file << "w0_bandwidth_hz = -10\n";
    file << "dsp_agc = slow\n";
    file << "stereo_blend = aggressive\n";
    file << "stereo = no\n";
    file.close();

    const bool result = config.loadFromFile("test_config.ini");
    REQUIRE(result == true);
    REQUIRE(config.processing.dsp_block_samples == 32768);
    REQUIRE(config.processing.w0_bandwidth_hz == 0);
    REQUIRE(config.processing.dsp_agc == "slow");
    REQUIRE(config.processing.stereo_blend == "aggressive");
    REQUIRE(config.processing.stereo == false);

    std::remove("test_config.ini");
}

TEST_CASE("Config parses low latency IQ toggle", "[config]") {
    Config config;
    config.loadDefaults();

    std::ofstream file("test_config.ini");
    file << "[sdr]\n";
    file << "low_latency_iq = true\n";
    file.close();

    const bool result = config.loadFromFile("test_config.ini");
    REQUIRE(result == true);
    REQUIRE(config.sdr.low_latency_iq == true);

    std::remove("test_config.ini");
}

TEST_CASE("Config accepts mixed-case booleans and preserves defaults on invalid enums", "[config]") {
    Config config;
    config.loadDefaults();

    std::ofstream file("test_config.ini");
    file << "[audio]\n";
    file << "enable_audio = On\n";
    file << "[processing]\n";
    file << "dsp_agc = turbo\n";
    file << "stereo_blend = weird\n";
    file.close();

    const bool result = config.loadFromFile("test_config.ini");
    REQUIRE(result == true);
    REQUIRE(config.audio.enable_audio == true);
    REQUIRE(config.processing.dsp_agc == "off");
    REQUIRE(config.processing.stereo_blend == "normal");

    std::remove("test_config.ini");
}
