#ifndef FM_TUNER_CALIBRATION_H
#define FM_TUNER_CALIBRATION_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace fm_tuner::calibration {

struct BandSweepOptions {
  uint32_t deviceIndex = 0;
  uint32_t sampleRateHz = 256000;
  uint32_t startKHz = 87500;
  uint32_t endKHz = 108000;
  uint32_t stepKHz = 100;
  int gainDb = -1; // -1 = auto-gain
};

struct Measurement {
  uint32_t freqKHz = 0;
  double dbfs = -120.0;
  double compensatedDbfs = -120.0;
  double clipRatio = 0.0;
};

struct Recommendation {
  bool valid = false;
  int floorDbfs = -45;
  int ceilDbfs = -1;
  size_t saturatedCount = 0;
  double minimumDbfs = -120.0;
  double p05Dbfs = -120.0;
  double medianDbfs = -120.0;
  double p95Dbfs = -120.0;
  double maximumDbfs = -120.0;
};

// Pure helpers used by the hardware sweep and its deterministic tests.
std::vector<uint32_t> makeCaptureCenters(uint32_t startHz, uint32_t endHz,
                                         uint32_t gridStepHz,
                                         uint32_t captureRateHz,
                                         uint32_t measurementWindowHz);
Recommendation recommendWindow(const std::vector<Measurement> &measurements);

// Sweeps the FM band on an attached RTL-SDR, prints a per-frequency table of
// raw dBFS / compensated dBFS / level120, and recommends signal_floor_dbfs /
// signal_ceil_dbfs values tuned to the observed envelope at this location.
// Returns 0 on success, non-zero on hardware failure.
int runBandSweep(const BandSweepOptions &options);

} // namespace fm_tuner::calibration

#endif
