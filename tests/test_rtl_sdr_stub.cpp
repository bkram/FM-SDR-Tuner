#include <catch2/catch_test_macros.hpp>

#include <array>

#include "rtl_sdr_device.h"

TEST_CASE("RTLSDRDevice stub behavior without librtlsdr backend",
          "[rtl_sdr_stub]") {
  RTLSDRDevice dev(0);

  // In test target we do not define FM_TUNER_HAS_RTLSDR, so all operations
  // are expected to fail gracefully without hardware.
  REQUIRE_FALSE(dev.connect());
  REQUIRE_FALSE(dev.setFrequency(101700000));
  REQUIRE_FALSE(dev.setSampleRate(256000));
  REQUIRE_FALSE(dev.setFrequencyCorrection(-30));
  REQUIRE_FALSE(dev.setGainMode(true));
  REQUIRE_FALSE(dev.setGain(300));
  REQUIRE_FALSE(dev.setAGC(true));

  std::array<uint8_t, 16> iq{};
  REQUIRE(dev.readIQ(iq.data(), iq.size() / 2) == 0);

  dev.setLowLatencyMode(true);
  dev.disconnect();
}
