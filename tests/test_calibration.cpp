#include "catch_compat.h"

#include "calibration.h"

#include <algorithm>
#include <cmath>
#include <vector>

TEST_CASE("Calibration wide captures cover every 100 kHz grid point",
          "[calibration]") {
  constexpr uint32_t startHz = 87500000;
  constexpr uint32_t endHz = 108000000;
  constexpr uint32_t stepHz = 100000;
  constexpr uint32_t captureRateHz = 1024000;
  constexpr uint32_t windowHz = 200000;
  const auto centers = fm_tuner::calibration::makeCaptureCenters(
      startHz, endHz, stepHz, captureRateHz, windowHz);

  REQUIRE(centers.size() == 35);
  const int64_t usableHalfSpan =
      static_cast<int64_t>(captureRateHz * 0.425) - windowHz / 2;
  for (uint32_t frequency = startHz; frequency <= endHz; frequency += stepHz) {
    const bool covered =
        std::any_of(centers.begin(), centers.end(), [&](uint32_t center) {
          return std::llabs(static_cast<int64_t>(frequency) - center) <=
                 usableHalfSpan;
        });
    REQUIRE(covered);
  }
}

TEST_CASE("Calibration recommends from clean compensated measurements",
          "[calibration]") {
  std::vector<fm_tuner::calibration::Measurement> measurements;
  for (int i = 0; i < 20; ++i) {
    measurements.push_back(
        {static_cast<uint32_t>(87500 + i * 100), 100.0 + i, -60.0 + i, 0.0});
  }
  measurements.push_back({90000, -1.0, -1.0, 0.5});

  const auto result = fm_tuner::calibration::recommendWindow(measurements);
  REQUIRE(result.valid);
  REQUIRE(result.saturatedCount == 1);
  REQUIRE(result.minimumDbfs == -60.0);
  REQUIRE(result.maximumDbfs == -41.0);
  REQUIRE(result.floorDbfs == -62);
  REQUIRE(result.ceilDbfs == -38);
}
