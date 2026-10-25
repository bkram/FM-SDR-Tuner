#ifndef SIGNAL_LEVEL_H
#define SIGNAL_LEVEL_H

#include <complex>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "dsp/liquid_primitives.h"

namespace fm_tuner {

// The width of the measurement window that the scan engine integrates around
// every point of its grid. (The tuned meter integrates the whole channel; see
// kTunedMeterWindowHz.)
//
// An FM station occupies far more spectrum than this: its carrier and its
// immediate sidebands (the part that carries the station's identity) span
// about 30 kHz, while the station's own skirt reaches out to +-100 kHz. A
// window much wider than 30 kHz would therefore measure the skirt of the
// neighbouring station and every point of the band would read as if a
// station were there. A window much narrower than the carrier's own energy
// would make the reading depend on the exact tuning.
//
// The scan integrates this window around every point of its grid.
constexpr int kSignalChannelWindowHz = 30000;

// The window the signal meter integrates around the tuned carrier: the whole
// channel. An FM signal has a constant envelope, so its total power does not
// depend on the programme; the 30 kHz scan window only holds the carrier's
// share of it, which falls as the modulation spreads the station across the
// channel. Measured with that window the meter pumped with the programme: the
// same carrier read -10.7 dBFS in silence and -19.0 dBFS at 75 kHz
// deviation. The scan and the meter therefore read the same number for an
// unmodulated carrier, and the meter reads the whole station when it is
// modulated.
constexpr int kTunedMeterWindowHz = 200000;

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
// Both surfaces read the same spectrum through this class. The scan
// integrates 30 kHz around each grid point and the meter the whole channel,
// so an unmodulated carrier shows the same number on both, and a modulated
// station reads higher on the meter. The scan engine uses it to measure the band power around
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

  // How the IQ's DC offset is removed before the transform.
  //  - BlockMean subtracts each block's own mean. Right for the scan, whose
  //    captures are short and placed so no station sits at DC.
  //  - Tracked subtracts a slow average across blocks (time constant
  //    kDcTrackingTimeConstantS). Right for the tuned meter: the tuned
  //    station sits at DC, and in a quiet passage its carrier is nearly
  //    constant over one block, so subtracting the block mean removed the
  //    carrier itself (a station within ~20 Hz of the tuned frequency read
  //    -56.6 dBFS instead of -10.5). A slow average still removes the
  //    tuner's static DC offset, but follows a carrier that is off by a few
  //    Hz far too slowly to cancel it.
  enum class DcRemoval { BlockMean, Tracked };
  static constexpr double kDcTrackingTimeConstantS = 0.5;
  void setDcRemoval(DcRemoval mode) { m_dcRemoval = mode; }
  // Forgets the tracked DC estimate (after a retune, or between tests).
  void resetDcEstimate();

  // Total power of the band [refHz +- bandwidthHz/2]. A band wider than the
  // analysed spectrum reads the whole spectrum; the band never wraps around
  // Nyquist. The DC offset is removed before the transform (see DcRemoval),
  // so the tuner's own DC offset needs no separate guard.
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
  DcRemoval m_dcRemoval = DcRemoval::BlockMean;
  bool m_dcValid = false;
  double m_dcI = 0.0;
  double m_dcQ = 0.0;
};

SignalLevelResult computeSignalLevel(const uint8_t *iq, size_t samples,
                                     int appliedGainDb, double gainCompFactor,
                                     double signalBiasDb, double floorDbfs,
                                     double ceilDbfs,
                                     uint32_t sampleRateHz = 0);

// Forgets the DC offset estimate the signal meter tracks across blocks on
// the calling thread. Call it after a retune so the previous station's DC
// component does not linger in the next one's reading.
void resetSignalLevelDcTracking();

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
