#include "signal_level.h"

#include "dsp/iq_saturation.h"
#include "dsp/liquid_primitives.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr double kWindowFloor = 1e-12;
constexpr double kPowerFloor = 1e-20;
constexpr double kSignalLevelSnrGateDb = 3.0;
constexpr double kSignalLevelSnrCeilDb = 30.0;

// Map a signed FFT bin offset onto the [0, nfft) range. The scan engine uses
// the same wrapping for its own estimator; keeping the two consistent means
// an index can never run off the end of the transform output.
size_t wrapBin(int bin, size_t nfft) {
  int wrapped = bin % static_cast<int>(nfft);
  if (wrapped < 0) {
    wrapped += static_cast<int>(nfft);
  }
  return static_cast<size_t>(wrapped);
}

size_t nearestPow2(size_t n) {
  size_t p = 1;
  while ((p << 1U) <= n) {
    p <<= 1U;
  }
  return p;
}

struct ChannelPowerEstimate {
  bool valid = false;
  double channelDbfs = -120.0;
  double noiseFloorDbfs = -120.0;
};

struct SignalMeterFftCache {
  ~SignalMeterFftCache() {
    if (plan != nullptr) {
      fft_destroy_plan(plan);
    }
  }

  bool ensureSize(size_t requestedNfft) {
    if (nfft == requestedNfft && plan != nullptr) {
      return true;
    }
    if (plan != nullptr) {
      fft_destroy_plan(plan);
      plan = nullptr;
    }
    nfft = requestedNfft;
    fftIn.assign(nfft, {});
    fftOut.assign(nfft, {});
    plan = fft_create_plan(static_cast<unsigned int>(nfft), fftIn.data(),
                           fftOut.data(), LIQUID_FFT_FORWARD, 0);
    return plan != nullptr;
  }

  size_t nfft = 0;
  std::vector<std::complex<float>> fftIn{};
  std::vector<std::complex<float>> fftOut{};
  fftplan plan = nullptr;
};

ChannelPowerEstimate estimateCenteredChannelPower(const uint8_t *iq,
                                                  size_t samples,
                                                  uint32_t sampleRateHz,
                                                  int channelBandwidthHz) {
  ChannelPowerEstimate out{};
  if (!iq || samples == 0 || sampleRateHz == 0 || channelBandwidthHz <= 0) {
    return out;
  }

  const size_t nfft = nearestPow2(std::min<size_t>(samples, 8192));
  if (nfft < 1024) {
    return out;
  }

  const float binHz =
      static_cast<float>(sampleRateHz) / static_cast<float>(nfft);
  if (binHz <= 0.0f) {
    return out;
  }

  const int bandHalf = std::max(
      1, static_cast<int>(std::lround((static_cast<float>(channelBandwidthHz) * 0.5f) / binHz)));
  const int dcRejectBins =
      std::max(1, static_cast<int>(std::lround(4000.0f / binHz)));
  const int guardBins =
      std::max(1, static_cast<int>(std::lround(4000.0f / binHz)));
  if (bandHalf <= dcRejectBins) {
    return out;
  }
  const int maxBin = static_cast<int>(nfft) - 1;
  if (bandHalf > maxBin) {
    return out;
  }
  // The side band measures the noise *away* from the channel. Its window
  // spans magnitudes [bandHalf + guard, bandHalf + guard + sideSpan]. The
  // negative-frequency half of that window must stay below the transform's
  // Nyquist point (nfft/2): once it passes that, it aliases onto the
  // positive half of the spectrum where the station sits, the "noise"
  // estimate picks up the signal itself, the noise floor collapses onto the
  // signal power, the SNR pins to 0 and a strong station reads as no signal
  // at all.
  //
  // The window does not have to be as wide as the channel — it only needs
  // enough bins for a stable noise estimate — so it is *capped* to whatever
  // fits below Nyquist rather than being dropped entirely. The mean is over
  // the same number of bins on both sides, so the noise power is still
  // estimated per bin and scaled to the channel width.
  const int nyquistBin = static_cast<int>(nfft) / 2;
  const int sideSpanBins = std::max(
      bandHalf, static_cast<int>(std::lround((static_cast<float>(channelBandwidthHz) * 0.5f) / binHz)));
  const int maxSideSpan = nyquistBin - bandHalf - guardBins;
  if (maxSideSpan <= 0) {
    // The channel itself already reaches Nyquist: no side band exists.
    return out;
  }
  const int effectiveSideSpan = std::min(sideSpanBins, maxSideSpan);
  const int sideOuter = bandHalf + guardBins + effectiveSideSpan;
  if (sideOuter > maxBin) {
    return out;
  }

  thread_local SignalMeterFftCache cache;
  if (!cache.ensureSize(nfft)) {
    return out;
  }

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
    const float w = 0.5f - 0.5f * std::cos((2.0f * kPi * static_cast<float>(i)) /
                                           static_cast<float>(nfft - 1));
    cache.fftIn[i] = {iRaw * w, qRaw * w};
  }

  fft_execute(cache.plan);

  double channelSum = 0.0;
  int channelBins = 0;
  for (int b = -bandHalf; b <= bandHalf; b++) {
    if (std::abs(b) <= dcRejectBins) {
      continue;
    }
    const size_t idx = wrapBin(b, nfft);
    const float re = cache.fftOut[idx].real();
    const float im = cache.fftOut[idx].imag();
    channelSum += static_cast<double>(re) * static_cast<double>(re) +
                  static_cast<double>(im) * static_cast<double>(im);
    channelBins++;
  }

  double sideSum = 0.0;
  int sideBins = 0;
  for (int sign : {-1, 1}) {
    const int start = sign * (bandHalf + guardBins);
    const int stop = sign * (bandHalf + guardBins + effectiveSideSpan);
    const int step = (start <= stop) ? 1 : -1;
    for (int b = start; b != stop + step; b += step) {
      const size_t idx = wrapBin(b, nfft);
      const float re = cache.fftOut[idx].real();
      const float im = cache.fftOut[idx].imag();
      sideSum += static_cast<double>(re) * static_cast<double>(re) +
                 static_cast<double>(im) * static_cast<double>(im);
      sideBins++;
    }
  }

  if (channelBins <= 0) {
    return out;
  }

  const double nfftNorm =
      static_cast<double>(nfft) * static_cast<double>(nfft);
  const double channelPower =
      std::max(kPowerFloor, channelSum / nfftNorm);
  const double sideMeanPowerPerBin =
      std::max(kPowerFloor, (sideBins > 0)
                                ? ((sideSum / static_cast<double>(sideBins)) /
                                   nfftNorm)
                                : (channelPower / static_cast<double>(channelBins)));
  const double sidePower =
      std::max(kPowerFloor,
               sideMeanPowerPerBin * static_cast<double>(channelBins));

  out.valid = true;
  out.channelDbfs = 10.0 * std::log10(channelPower + kWindowFloor);
  out.noiseFloorDbfs = 10.0 * std::log10(sidePower + kWindowFloor);
  return out;
}

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
                                     double ceilDbfs, uint32_t sampleRateHz,
                                     int channelBandwidthHz) {
  SignalLevelResult out = computeWidebandSignalLevel(iq, samples);
  if (!iq || samples == 0) {
    return out;
  }

  const ChannelPowerEstimate channel = estimateCenteredChannelPower(
      iq, samples, sampleRateHz, channelBandwidthHz);
  if (channel.valid) {
    out.dbfs = channel.channelDbfs;
    out.noiseFloorDbfs = channel.noiseFloorDbfs;
    const double channelPower = std::pow(10.0, out.dbfs / 10.0);
    const double noisePower = std::pow(10.0, out.noiseFloorDbfs / 10.0);
    const double signalExcessPower = std::max(kPowerFloor, channelPower - noisePower);
    out.snrDb = std::max(0.0,
                         10.0 * std::log10((signalExcessPower + kPowerFloor) /
                                           (noisePower + kPowerFloor)));
  }
  out.compensatedDbfs = out.dbfs -
                        (static_cast<double>(appliedGainDb) * gainCompFactor) +
                        signalBiasDb;
  out.level120 = computeDisplaySignalLevel120(
      out.dbfs, out.noiseFloorDbfs, appliedGainDb, gainCompFactor, signalBiasDb,
      floorDbfs, ceilDbfs, channel.valid);
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
