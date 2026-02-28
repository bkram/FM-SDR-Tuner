#ifndef SIGNAL_LEVEL_H
#define SIGNAL_LEVEL_H

#include <cstddef>
#include <cstdint>

struct SignalLevelResult {
  float level120 = 0.0f;
  double dbfs = -120.0;
  double compensatedDbfs = -120.0;
  double hardClipRatio = 0.0;
  double nearClipRatio = 0.0;
};

struct SignalLevelSmoother {
  bool initialized = false;
  float value = 0.0f;
};

SignalLevelResult computeSignalLevel(const uint8_t *iq, size_t samples,
                                    int appliedGainDb, double gainCompFactor,
                                    double signalBiasDb, double floorDbfs,
                                    double ceilDbfs);

float smoothSignalLevel(float input, SignalLevelSmoother &state);

#endif
