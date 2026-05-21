#ifndef FM_TUNER_CALIBRATION_H
#define FM_TUNER_CALIBRATION_H

#include <cstdint>

namespace fm_tuner::calibration {

struct BandSweepOptions {
  uint32_t deviceIndex = 0;
  uint32_t sampleRateHz = 256000;
  uint32_t startKHz = 87500;
  uint32_t endKHz = 108000;
  uint32_t stepKHz = 100;
  int gainTenthsDb = -1; // -1 = auto-gain
  int channelBandwidthHz = 194000;
};

// Sweeps the FM band on an attached RTL-SDR, prints a per-frequency table of
// raw dBFS / compensated dBFS / level120, and recommends signal_floor_dbfs /
// signal_ceil_dbfs values tuned to the observed envelope at this location.
// Returns 0 on success, non-zero on hardware failure.
int runBandSweep(const BandSweepOptions &options);

} // namespace fm_tuner::calibration

#endif
