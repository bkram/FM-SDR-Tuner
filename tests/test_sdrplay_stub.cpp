#include "catch_compat.h"

#include <array>
#include <complex>

#include "sdrplay_device.h"
#include "tuner_controller.h"

// These tests compile WITHOUT FM_TUNER_HAS_SDRPLAY, exercising the stub path:
// the device reports unavailable and a "sdrplay" TunerController degrades
// gracefully (connect fails, no crash) so the app can fall back to RTL.

TEST_CASE("SDRplayDevice stub reports unavailable", "[sdrplay_stub]") {
  REQUIRE_FALSE(SDRplayDevice::apiAvailable());
  REQUIRE(SDRplayDevice::deviceCount() == 0);

  SDRplayDevice dev;
  REQUIRE_FALSE(dev.connect());
  REQUIRE_FALSE(dev.setFrequency(98000000));
  REQUIRE_FALSE(dev.setGain(300));
  REQUIRE_FALSE(dev.setAGC(true));
  REQUIRE_FALSE(dev.setLnaState(2));
  REQUIRE_FALSE(dev.setAntenna(1));
  REQUIRE_FALSE(dev.setBiasTee(true));
  REQUIRE_FALSE(dev.setScanWideMode(true));
  REQUIRE(dev.antennaCount() == 1);

  std::array<std::complex<float>, 16> cf{};
  REQUIRE(dev.readIQ(cf.data(), cf.size()) == 0);
  std::array<uint8_t, 32> u8{};
  REQUIRE(dev.readIQ(u8.data(), 16) == 0);
}

TEST_CASE("TunerController reports sdrplay as a CF32 source", "[sdrplay_stub]") {
  TunerController tuner("sdrplay", "", 0, 0);
  REQUIRE(tuner.isSdrPlay());
  REQUIRE_FALSE(tuner.isDirectRtlSdr());
  REQUIRE(tuner.nativeFormat() == TunerController::IqFormat::CF32);
  REQUIRE(std::string(tuner.name()) == "sdrplay");
  // Stub: connect fails so the caller can fall back to another source.
  REQUIRE_FALSE(tuner.connect());
}

TEST_CASE("TunerController scan wide mode is SDRplay-only", "[sdrplay_stub]") {
  // RTL sources report 0 (no wide mode) so the scan keeps its normal rate.
  TunerController rtl("rtl_sdr", "", 0, 0);
  REQUIRE(rtl.setScanWideMode(true) == 0u);
  REQUIRE(rtl.setScanWideMode(false) == 0u);
}

TEST_CASE("TunerController RTL sources are U8", "[sdrplay_stub]") {
  TunerController rtl("rtl_sdr", "", 0, 0);
  REQUIRE(rtl.nativeFormat() == TunerController::IqFormat::U8);
  REQUIRE_FALSE(rtl.isSdrPlay());

  TunerController tcp("rtl_tcp", "localhost", 1234, 0);
  REQUIRE(tcp.nativeFormat() == TunerController::IqFormat::U8);
  // SDRplay-only knobs are no-ops on RTL.
  REQUIRE_FALSE(rtl.setLnaState(2));
  REQUIRE_FALSE(rtl.setAntenna(1));
  REQUIRE(rtl.antennaCount() == 1);
}
