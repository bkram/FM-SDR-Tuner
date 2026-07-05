#ifndef SIGNAL_LEVEL_H
#define SIGNAL_LEVEL_H

#include <cstddef>
#include <cstdint>

struct SignalLevelResult {
  float level120 = 0.0f;
  double dbfs = -120.0;
  double compensatedDbfs = -120.0;
  double noiseFloorDbfs = -120.0;
  double snrDb = 0.0;
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
                                     double ceilDbfs,
                                     uint32_t sampleRateHz = 0,
                                     int channelBandwidthHz = 0);

float computeDisplaySignalLevel120(double channelDbfs, double noiseFloorDbfs,
                                   int appliedGainDb, double gainCompFactor,
                                   double signalBiasDb, double floorDbfs,
                                   double ceilDbfs, bool channelAware);

// Maps a channel SNR (dB) onto the 0-120 meter scale using the same gate/ceil
// window the channel-aware absolute mapping uses. A non-finite snrDb means "no
// SNR estimate" and returns 120 (no cap). Exposed so the display path can cap
// its absolute level by the FFT-derived SNR even when it sources the absolute
// term from the demodulator's channel-power estimate.
float snrLevel120FromSnrDb(double snrDb);

float smoothSignalLevel(float input, SignalLevelSmoother &state);

#endif
