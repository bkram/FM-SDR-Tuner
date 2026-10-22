#ifndef SIGNAL_LEVEL_H
#define SIGNAL_LEVEL_H

#include <complex>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "dsp/liquid_primitives.h"

namespace fm_tuner {

// The width of the measurement window that the signal meter and the scan
// engine both integrate around the point they are looking at.
//
// An FM station occupies far more spectrum than this: its carrier and its
// immediate sidebands (the part that carries the station's identity) span
// about 30 kHz, while the station's own skirt reaches out to +-100 kHz. A
// window much wider than 30 kHz would therefore measure the skirt of the
// neighbouring station and every point of the band would read as if a
// station were there. A window much narrower than the carrier's own energy
// would make the reading depend on the exact tuning.
//
// Both surfaces use this one window, so the number the meter shows for the
// tuned station is the number the scan reports for the same station.
constexpr int kSignalChannelWindowHz = 30000;

} // namespace fm_tuner

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

// One windowed FFT of an IQ block, plus the measurements the signal meter and
// the scan engine take from it.
//
// Both surfaces read the same spectrum through this class and integrate the
// same window, so a station shows the same number on the meter as on the
// spectrum plot. The scan engine uses it to measure the band power around
// every point of a fine frequency grid; the meter uses it to measure the
// band power around the tuned carrier.
class IqSpectrum {
public:
  IqSpectrum();
  ~IqSpectrum();
  IqSpectrum(const IqSpectrum &) = delete;
  IqSpectrum &operator=(const IqSpectrum &) = delete;

  // Transforms `samples` interleaved uint8 I/Q samples. Returns false when
  // the block is too short to transform.
  bool analyse(const uint8_t *iq, size_t samples, uint32_t sampleRateHz);

  bool valid() const { return m_valid; }
  size_t nfft() const { return m_nfft; }
  double binHz() const { return m_binHz; }
  // Half of the analysed spectrum, in Hz: the largest offset the block
  // carries.
  double halfSpanHz() const;

  // How many FFT plans this object has created. The scan re-uses one
  // spectrum object for every block, so this only grows when the block size
  // changes; it is here to make that visible.
  uint32_t planCreations() const { return m_planCreations; }

  // Total power of the band [refHz +- bandwidthHz/2]. A band wider than the
  // analysed spectrum reads the whole spectrum; the band never wraps around
  // Nyquist. The block's mean is removed before the transform, so the
  // tuner's own DC offset needs no separate guard.
  double channelPower(int64_t refHz, int bandwidthHz) const;
  double channelDbfs(int64_t refHz, int bandwidthHz) const;

  // Mean power per bin of the annulus around refHz, in dBFS. This is the
  // local noise reference: an absolute noise reference is useless because the
  // tuner's analogue response ripples by tens of dB across the band.
  double localNoiseDbfs(int64_t refHz, int annInnerHz, int annOuterHz) const;

private:
  // Index into the ascending-frequency power array.
  size_t binIndex(int64_t offsetHz) const;
  double rangeMeanDbfs(int64_t lowHz, int64_t highHz) const;

  uint32_t m_sampleRateHz = 0;
  size_t m_nfft = 0;
  double m_binHz = 0.0;
  bool m_valid = false;
  // Per-bin power in ascending frequency order (index 0 is the most negative
  // offset), plus its running sum for the range queries.
  std::vector<double> m_power;
  std::vector<double> m_prefix;
  std::vector<std::complex<float>> m_fftIn;
  std::vector<std::complex<float>> m_fftOut;
  std::vector<float> m_window;
  fftplan m_plan = nullptr;
  uint32_t m_planCreations = 0;
};

SignalLevelResult computeSignalLevel(const uint8_t *iq, size_t samples,
                                     int appliedGainDb, double gainCompFactor,
                                     double signalBiasDb, double floorDbfs,
                                     double ceilDbfs,
                                     uint32_t sampleRateHz = 0);

float computeDisplaySignalLevel120(double channelDbfs, double noiseFloorDbfs,
                                   int appliedGainDb, double gainCompFactor,
                                   double signalBiasDb, double floorDbfs,
                                   double ceilDbfs, bool channelAware);

// Maps a channel SNR (dB) onto the 0-120 meter scale using the same gate/ceil
// window the channel-aware absolute mapping uses. A non-finite snrDb means "no
// SNR estimate" and returns 120 (no cap).
float snrLevel120FromSnrDb(double snrDb);

float smoothSignalLevel(float input, SignalLevelSmoother &state);

// True when the front end is probably being driven into saturation by the
// station that is tuned: the ADC is clipping while the channel is strong.
//
// A saturated front end mixes the strong station with itself and with its
// neighbours. The intermodulation products land at other frequencies, where
// they are heard (and plotted) as a duplicate of a station -- the classic
// "I hear the same station twice" symptom. The condition needs all three
// parts: clipping (the front end is saturated), a strong channel (there is
// something strong enough to saturate it with) and a high meter reading (the
// station really is loud).
bool suspectOverloadGhosting(const SignalLevelResult &signal, double clipRatio);

#endif
