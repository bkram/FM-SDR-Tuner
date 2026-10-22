#include "signal_level.h"

#include "dsp/iq_saturation.h"
#include "dsp/liquid_primitives.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>
#include <vector>

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr double kWindowFloor = 1e-12;
constexpr double kPowerFloor = 1e-20;
constexpr double kSignalLevelSnrGateDb = 3.0;
constexpr double kSignalLevelSnrCeilDb = 30.0;
// The noise reference is taken from a wide annulus around the measured
// channel. A station's own skirt reaches out to +-100 kHz, so an annulus
// right next to the channel would measure the station instead of the noise;
// the wide annulus only holds the noise where the station is weak.
constexpr int kNoiseAnnulusInnerHz = 60000;
constexpr int kNoiseAnnulusOuterHz = 90000;
// Distance between the edge of the channel and the annulus used when the
// analysed spectrum is too narrow to hold the wide one.
constexpr int kNoiseBandGuardHz = 6000;

size_t nearestPow2(size_t n) {
  size_t p = 1;
  while ((p << 1U) <= n) {
    p <<= 1U;
  }
  return p;
}

} // namespace

// The Hann window is applied to the input, so the mean power of the spectrum
// is sum(|X|^2)/(N * sum(w^2)), not sum(|X|^2)/N^2: sum(w^2) is 0.375*N for a
// Hann window. Without the window factor the estimate reads about 4.26 dB too
// low (measured against the time-domain mean power of the same filtered IQ).
constexpr double kHannWindowPowerFactor = 0.375;

IqSpectrum::IqSpectrum() = default;

IqSpectrum::~IqSpectrum() {
  if (m_plan != nullptr) {
    fft_destroy_plan(m_plan);
    m_plan = nullptr;
  }
}

bool IqSpectrum::analyse(const uint8_t *iq, size_t samples,
                          uint32_t sampleRateHz) {
  m_valid = false;
  if (!iq || samples == 0 || sampleRateHz == 0) {
    return false;
  }

  const size_t nfft = nearestPow2(std::min<size_t>(samples, 32768));
  if (nfft < 1024) {
    return false;
  }
  if (m_nfft != nfft || m_plan == nullptr) {
    if (m_plan != nullptr) {
      fft_destroy_plan(m_plan);
      m_plan = nullptr;
    }
    m_nfft = nfft;
    m_fftIn.assign(nfft, {});
    m_fftOut.assign(nfft, {});
    m_window.assign(nfft, 0.0f);
    for (size_t i = 0; i < nfft; i++) {
      m_window[i] = 0.5f - 0.5f * std::cos((2.0f * kPi *
                                            static_cast<float>(i)) /
                                           static_cast<float>(nfft - 1));
    }
    m_plan = fft_create_plan(static_cast<unsigned int>(nfft), m_fftIn.data(),
                              m_fftOut.data(), LIQUID_FFT_FORWARD, 0);
    if (m_plan == nullptr) {
      m_nfft = 0;
      return false;
    }
  }

  m_sampleRateHz = sampleRateHz;
  m_binHz = static_cast<double>(sampleRateHz) / static_cast<double>(nfft);

  double meanI = 0.0;
  double meanQ = 0.0;
  for (size_t i = 0; i < nfft; i++) {
    meanI += (static_cast<int>(iq[i * 2]) - 127.5) * (1.0 / 127.5);
    meanQ += (static_cast<int>(iq[i * 2 + 1]) - 127.5) * (1.0 / 127.5);
  }
  meanI /= static_cast<double>(nfft);
  meanQ /= static_cast<double>(nfft);

  for (size_t i = 0; i < nfft; i++) {
    const float iRaw = static_cast<float>(
        (static_cast<int>(iq[i * 2]) - 127.5) * (1.0 / 127.5) - meanI);
    const float qRaw = static_cast<float>(
        (static_cast<int>(iq[i * 2 + 1]) - 127.5) * (1.0 / 127.5) - meanQ);
    m_fftIn[i] = {iRaw * m_window[i], qRaw * m_window[i]};
  }

  fft_execute(m_plan);

  // The transform output runs from DC up to Nyquist and then wraps to the
  // negative frequencies. Copy it into an ascending-frequency array so the
  // band and annulus queries are plain ranges.
  const size_t half = nfft / 2;
  m_power.assign(nfft, 0.0);
  const double norm = 1.0 / (kHannWindowPowerFactor *
                              static_cast<double>(nfft) *
                              static_cast<double>(nfft));
  for (size_t j = 0; j < nfft; j++) {
    const int64_t offset = (j < half) ? static_cast<int64_t>(j)
                                       : (static_cast<int64_t>(j) -
                                          static_cast<int64_t>(nfft));
    const size_t idx = static_cast<size_t>(offset + static_cast<int64_t>(half));
    const float re = m_fftOut[j].real();
    const float im = m_fftOut[j].imag();
    m_power[idx] = (static_cast<double>(re) * static_cast<double>(re) +
                     static_cast<double>(im) * static_cast<double>(im)) *
                    norm;
  }
  m_prefix.assign(nfft + 1, 0.0);
  for (size_t i = 0; i < nfft; i++) {
    m_prefix[i + 1] = m_prefix[i] + m_power[i];
  }
  m_valid = true;
  return true;
}

double IqSpectrum::halfSpanHz() const {
  if (!m_valid) {
    return 0.0;
  }
  return static_cast<double>(m_nfft / 2) * m_binHz;
}

size_t IqSpectrum::binIndex(int64_t offsetHz) const {
  const int64_t half = static_cast<int64_t>(m_nfft / 2);
  int64_t idx = static_cast<int64_t>(std::llround(static_cast<double>(offsetHz) /
                                                  m_binHz)) +
                half;
  if (idx < 0) {
    idx = 0;
  }
  if (idx > static_cast<int64_t>(m_nfft) - 1) {
    idx = static_cast<int64_t>(m_nfft) - 1;
  }
  return static_cast<size_t>(idx);
}

double IqSpectrum::rangeMeanDbfs(int64_t lowHz, int64_t highHz) const {
  if (!m_valid || highHz < lowHz) {
    return -200.0;
  }
  const int64_t lo = std::max<int64_t>(
      0, static_cast<int64_t>(binIndex(lowHz)));
  const int64_t hi = std::min<int64_t>(static_cast<int64_t>(m_nfft) - 1,
                                       static_cast<int64_t>(binIndex(highHz)));
  if (hi < lo) {
    return -200.0;
  }
  const double sum = m_prefix[static_cast<size_t>(hi + 1)] - m_prefix[static_cast<size_t>(lo)];
  const double count = static_cast<double>(hi - lo + 1);
  return 10.0 * std::log10(std::max(kPowerFloor, sum / count) + kWindowFloor);
}

double IqSpectrum::channelPower(int64_t refHz, int bandwidthHz) const {
  if (!m_valid || bandwidthHz <= 0) {
    return 0.0;
  }
  const int64_t half = static_cast<int64_t>(bandwidthHz) / 2;
  // The band is clipped to the analysed spectrum: a band wider than the
  // capture reads the whole capture, and a band that would reach past
  // Nyquist is cut off instead of wrapping onto the other side of the band.
  const int64_t lo = binIndex(refHz - half);
  const int64_t hi = binIndex(refHz + half);
  if (hi < lo) {
    return 0.0;
  }
  const double sum = m_prefix[static_cast<size_t>(hi + 1)] -
                     m_prefix[static_cast<size_t>(lo)];
  return std::max(0.0, sum);
}

double IqSpectrum::channelDbfs(int64_t refHz, int bandwidthHz) const {
  return 10.0 * std::log10(channelPower(refHz, bandwidthHz) + kWindowFloor);
}

double IqSpectrum::localNoiseDbfs(int64_t refHz, int annInnerHz,
                                  int annOuterHz) const {
  if (!m_valid || annOuterHz <= annInnerHz) {
    return -120.0;
  }
  // The mean power per bin of the two annulus halves, averaged together.
  const double low = std::pow(
      10.0, rangeMeanDbfs(refHz - annOuterHz, refHz - annInnerHz) / 10.0);
  const double high = std::pow(
      10.0, rangeMeanDbfs(refHz + annInnerHz, refHz + annOuterHz) / 10.0);
  return 10.0 * std::log10(std::max(kPowerFloor, (low + high) * 0.5) +
                           kWindowFloor);
}

namespace {

SignalLevelResult computeWidebandSignalLevel(const uint8_t *iq, size_t samples) {
  SignalLevelResult out{};
  if (!iq || samples == 0) {
    return out;
  }

  double sumI = 0.0;
  double sumQ = 0.0;
  double sumII = 0.0;
  double sumQQ = 0.0;
  size_t hardClipCount = 0;
  size_t nearClipCount = 0;

  for (size_t s = 0; s < samples; s++) {
    const uint8_t iByte = iq[2 * s];
    const uint8_t qByte = iq[2 * s + 1];
    const double iNorm = (static_cast<double>(iByte) - 127.5) * (1.0 / 127.5);
    const double qNorm = (static_cast<double>(qByte) - 127.5) * (1.0 / 127.5);

    sumI += iNorm;
    sumQ += qNorm;
    sumII += iNorm * iNorm;
    sumQQ += qNorm * qNorm;

    // The clip ratios are fractions of *bytes* (the denominator below is
    // samples * 2), so each byte is counted individually. Counting 2 for a
    // sample whose I byte alone was saturated reported twice the true
    // fraction, which crossed the 2% auto-gain step-down threshold for a
    // station that was never actually overloaded.
    if (fm_tuner::dsp::isRtlSdrIqByteSaturated(iByte)) {
      hardClipCount += 1;
    }
    if (fm_tuner::dsp::isRtlSdrIqByteSaturated(qByte)) {
      hardClipCount += 1;
    }
    if (iByte <= 8 || iByte >= 247) {
      nearClipCount += 1;
    }
    if (qByte <= 8 || qByte >= 247) {
      nearClipCount += 1;
    }
  }

  const double n = static_cast<double>(samples);
  const double meanI = sumI / n;
  const double meanQ = sumQ / n;
  const double varI = std::max(0.0, (sumII / n) - (meanI * meanI));
  const double varQ = std::max(0.0, (sumQQ / n) - (meanQ * meanQ));
  const double rms = std::sqrt(std::max(1e-15, 0.5 * (varI + varQ)));

  out.dbfs = 20.0 * std::log10(rms + 1e-12);
  out.noiseFloorDbfs = out.dbfs;
  // NaN signals "no channel-aware estimate available". Downstream consumers
  // (adaptive bandwidth controller, auto-gain) can use std::isfinite() to
  // distinguish a missing measurement from a genuine SNR ≈ 0 reading. If the
  // FFT-based estimator runs successfully it overrides this with a real value.
  out.snrDb = std::numeric_limits<double>::quiet_NaN();

  const double iqValues = static_cast<double>(samples * 2);
  out.hardClipRatio =
      (iqValues > 0.0) ? (static_cast<double>(hardClipCount) / iqValues) : 0.0;
  out.nearClipRatio =
      (iqValues > 0.0) ? (static_cast<double>(nearClipCount) / iqValues) : 0.0;
  return out;
}

} // namespace

SignalLevelResult computeSignalLevel(const uint8_t *iq, size_t samples,
                                     int appliedGainDb, double gainCompFactor,
                                     double signalBiasDb, double floorDbfs,
                                     double ceilDbfs, uint32_t sampleRateHz) {
  SignalLevelResult out = computeWidebandSignalLevel(iq, samples);
  if (!iq || samples == 0) {
    return out;
  }

  // The meter and the scan engine read the spectrum through the same
  // estimator and integrate the same window, so the number the meter shows
  // for a station is the number the scan reports for it.
  thread_local IqSpectrum spectrum;
  if (sampleRateHz > 0 && spectrum.analyse(iq, samples, sampleRateHz)) {
    const int windowHz = fm_tuner::kSignalChannelWindowHz;
    out.dbfs = spectrum.channelDbfs(0, windowHz);
    // The noise reference sits outside the channel. It has to be local: the
    // tuner's analogue response ripples by tens of dB across the band, so a
    // fixed or far-away reference would report a wildly wrong floor
    // depending on where in the band we are. It also has to be wide: a
    // station's own skirt reaches +-100 kHz, so an annulus right next to the
    // channel would measure the station instead of the noise. The reading is
    // therefore only meaningful next to a weak or empty channel - the
    // reception quality of a strong station comes from the demodulator
    // instead (see processing_runner).
    int annInner = kNoiseAnnulusInnerHz;
    int annOuter = kNoiseAnnulusOuterHz;
    if (spectrum.halfSpanHz() < static_cast<double>(annOuter)) {
      annInner = windowHz / 2 + kNoiseBandGuardHz;
      annOuter = annInner + windowHz / 2;
    }
    const double noisePerBin = spectrum.localNoiseDbfs(0, annInner, annOuter);
    const double binCount =
        static_cast<double>(windowHz) / std::max(1.0, spectrum.binHz());
    out.noiseFloorDbfs =
        noisePerBin + 10.0 * std::log10(std::max(1.0, binCount));
    const double channelPower = std::pow(10.0, out.dbfs / 10.0);
    const double noisePower = std::pow(10.0, out.noiseFloorDbfs / 10.0);
    const double signalExcessPower =
        std::max(kPowerFloor, channelPower - noisePower);
    out.snrDb = std::max(
        0.0, 10.0 * std::log10((signalExcessPower + kPowerFloor) /
                               (noisePower + kPowerFloor)));
  }
  out.compensatedDbfs = out.dbfs -
                        (static_cast<double>(appliedGainDb) * gainCompFactor) +
                        signalBiasDb;
  // The displayed level is the absolute strength of the tuned channel: the
  // scan reports the same absolute strength for the same station, so the two
  // surfaces can be read against each other. The reception quality of the
  // channel is a separate figure and comes from the demodulator.
  out.level120 = computeDisplaySignalLevel120(
      out.dbfs, out.noiseFloorDbfs, appliedGainDb, gainCompFactor, signalBiasDb,
      floorDbfs, ceilDbfs, false);
  return out;
}

float snrLevel120FromSnrDb(double snrDb) {
  if (!std::isfinite(snrDb)) {
    return 120.0f;
  }
  const double snrNorm = (snrDb - kSignalLevelSnrGateDb) /
                         (kSignalLevelSnrCeilDb - kSignalLevelSnrGateDb);
  return std::clamp(static_cast<float>(snrNorm * 120.0), 0.0f, 120.0f);
}

float computeDisplaySignalLevel120(double channelDbfs, double noiseFloorDbfs,
                                   int appliedGainDb, double gainCompFactor,
                                   double signalBiasDb, double floorDbfs,
                                   double ceilDbfs, bool channelAware) {
  const double compensatedDbfs =
      channelDbfs - (static_cast<double>(appliedGainDb) * gainCompFactor) +
      signalBiasDb;
  const double safeCeil = std::max(ceilDbfs, floorDbfs + 1.0);
  const double absNorm = (compensatedDbfs - floorDbfs) / (safeCeil - floorDbfs);
  float level120 =
      std::clamp(static_cast<float>(absNorm * 120.0), 0.0f, 120.0f);
  if (channelAware) {
    const double channelPower = std::pow(10.0, channelDbfs / 10.0);
    const double noisePower = std::pow(10.0, noiseFloorDbfs / 10.0);
    const double signalExcessPower = std::max(kPowerFloor, channelPower - noisePower);
    const double snrDb = std::max(
        0.0, 10.0 * std::log10((signalExcessPower + kPowerFloor) /
                               (noisePower + kPowerFloor)));
    level120 = std::min(level120, snrLevel120FromSnrDb(snrDb));
  }
  return level120;
}

float smoothSignalLevel(float input, SignalLevelSmoother &state) {
  if (!state.initialized) {
    state.value = input;
    state.initialized = true;
    return state.value;
  }
  const float alpha = (input > state.value) ? 0.42f : 0.18f;
  state.value += (input - state.value) * alpha;
  return state.value;
}

bool suspectOverloadGhosting(const SignalLevelResult &signal,
                             double clipRatio) {
  // The overload image only shows up when the front end is driven into
  // saturation by a strong signal, so the level has to be a genuinely strong
  // one. The threshold is on the uncompensated band power of the tuned
  // channel, which is the same figure the scan engine reports.
  return clipRatio >= 0.01 && signal.level120 >= 70.0f && signal.dbfs >= -20.0;
}
