#include "fm_demod.h"

#include "dsp/iq_saturation.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>

namespace {
// Starting step of the IQ DC tracker: a 20 Hz corner at 256 kHz, the corner
// the former fixed DC blocker used throughout.
constexpr double kIqDcAlphaFast = 0.0005;

constexpr std::array<int, 30> kXdrFmBwHz = {
    309000, 298000, 281000, 263000, 246000, 229000, 211000, 194000,
    177000, 159000, 142000, 125000, 108000, 95000,  90000,  83000,
    73000,  63000,  55000,  48000,  42000,  36000,  32000,  27000,
    24000,  20000,  17000,  15000,  9000,   0};

const std::array<float, 256> &iqNormLut() {
  static const std::array<float, 256> lut = []() {
    std::array<float, 256> out{};
    for (int v = 0; v < 256; v++) {
      out[static_cast<size_t>(v)] = (static_cast<float>(v) - 127.0f) / 127.5f;
    }
    return out;
  }();
  return lut;
}
} // namespace

FMDemod::FMDemod(int inputRate, int outputRate)
    : m_inputRate(std::max(1, inputRate)),
      m_outputRate(std::max(1, outputRate)), m_deviation(75000.0),
      m_deemphasisEnabled(true), m_bandwidthMode(0), m_w0BandwidthHz(194000),
      m_dspAgcMode(DspAgcMode::Off), m_clipping(false), m_clippingRatio(0.0f),
      m_filteredChannelPowerDbfs(-120.0) {
  const float iqCutoffNorm =
      std::clamp(110000.0f / static_cast<float>(m_inputRate), 0.01f, 0.45f);
  designIqFilter(81, iqCutoffNorm, 60.0f);
  // The IQ DC tracker settles to a 0.1 Hz corner over the first half second
  // after a reset (see removeIqDc()).
  constexpr double kIqDcSlowCornerHz = 0.1;
  constexpr double kIqDcGlideSeconds = 0.5;
  m_iqDcAlphaSlow = 2.0 * 3.14159265358979323846 * kIqDcSlowCornerHz /
                    static_cast<double>(m_inputRate);
  m_iqDcAlphaDecay =
      std::pow(m_iqDcAlphaSlow / kIqDcAlphaFast,
               1.0 / (kIqDcGlideSeconds * static_cast<double>(m_inputRate)));
  resetIqDc();
  const float ratio =
      static_cast<float>(m_outputRate) / static_cast<float>(m_inputRate);
  // The mono path (stereo = false) feeds the raw multiplex - pilot 19 kHz,
  // L-R subcarrier 23-53 kHz, RDS 57 kHz - into this resampler, so it is also
  // the audio anti-alias filter. liquid's default cutoff does not band-limit a
  // 5.33x downsample at all: the L-R sidebands folded to 9-11 kHz and the
  // pilot passed at full level. The filter is flat to 15 kHz and rejects the
  // pilot by 78 dB and everything above it by 90 dB. It is computed only at
  // the output rate, so the long filter costs ~15 M multiply-adds a second.
  constexpr float kMonoAudioCutoffHz = 17000.0f;
  constexpr std::uint32_t kMonoResampHalfLen = 160;
  constexpr float kMonoResampStopBandDb = 80.0f;
  constexpr std::uint32_t kMonoResampPolyphase = 64;
  const float monoCutoffNorm = std::min(
      0.45f, kMonoAudioCutoffHz / static_cast<float>(m_inputRate));
  m_liquidMonoResampler.init(ratio, kMonoResampHalfLen, monoCutoffNorm,
                             kMonoResampStopBandDb, kMonoResampPolyphase);
  m_liquidMonoDcBlock.initDCBlocker(0.0008f);
  setDeviation(75000.0);
  setDeemphasis(50);
  setDspAgcMode(DspAgcMode::Off);
  m_liquidMultipathEq.init(fm_tuner::dsp::MultipathEqMode::Off, 17,
                           static_cast<float>(m_inputRate));
}

FMDemod::~FMDemod() = default;

void FMDemod::setDeemphasis(int tau_us) {
  if (tau_us <= 0) {
    m_deemphasisEnabled = false;
    m_liquidMonoDeemphasis.reset();
    return;
  }
  m_deemphasisEnabled = true;
  // Pre-warped bilinear one-pole (the AF stage's design), run at the input
  // rate ahead of the resampler. At 256 kHz the bilinear transform's
  // frequency warping is negligible across the audio band, so the response
  // tracks the analog RC curve to within 0.1 dB up to 15 kHz. The former
  // Euler one-pole at 48 kHz was 0.7-1.0 dB low from 3 to 10 kHz.
  const float tau = static_cast<float>(tau_us) * 1e-6f;
  const float fs = static_cast<float>(m_inputRate);
  const float wpp = std::tan(1.0f / (2.0f * fs * tau));
  const float b0 = wpp / (wpp + 1.0f);
  const float a1 = (wpp - 1.0f) / (wpp + 1.0f);
  m_liquidMonoDeemphasis.init({b0, b0}, {1.0f, a1});
}

void FMDemod::setDeviation(double deviation) {
  m_deviation = deviation;
  // liquid freqdem expects kf in cycles/sample; match legacy gain:
  // out = delta_phase * Fs / (2*pi*deviation)
  // => kf = deviation / Fs
  m_liquidFreqDemod.init(
      static_cast<float>(m_deviation / static_cast<double>(m_inputRate)));
}

void FMDemod::reset() {
  m_clipping = false;
  m_clippingRatio = 0.0f;
  m_filteredChannelPowerDbfs = -120.0;
  m_liquidIqFilter.reset();
  m_iqHistory.fill(std::complex<float>(0.0f, 0.0f));
  m_iqHistoryPos = 0;
  m_liquidFreqDemod.reset();
  resetIqDc();
  if (m_deemphasisEnabled) {
    m_liquidMonoDeemphasis.reset();
  }
  m_liquidMonoDcBlock.reset();
  m_liquidMonoResampler.reset();
  if (m_liquidIqAgc.ready()) {
    m_liquidIqAgc.reset();
  }
  m_liquidMultipathEq.reset();
}

void FMDemod::setBandwidthMode(int mode) {
  static constexpr int kTefBwHz[] = {
      311000, 287000, 254000, 236000, 217000, 200000, 184000, 168000, 151000,
      133000, 114000, 97000,  84000,  72000,  64000,  56000,  0};
  const int clipped =
      std::clamp(mode, 0, static_cast<int>(std::size(kTefBwHz) - 1));
  setBandwidthHz(kTefBwHz[clipped]);
}

void FMDemod::setBandwidthHz(int bwHz) {
  const int effectiveBwHz = (bwHz <= 0) ? m_w0BandwidthHz : bwHz;
  int selected = static_cast<int>(kXdrFmBwHz.size() - 1);
  if (effectiveBwHz > 0) {
    int minDiff = std::numeric_limits<int>::max();
    for (int i = 0; i < static_cast<int>(kXdrFmBwHz.size()) - 1; i++) {
      const int diff =
          std::abs(kXdrFmBwHz[static_cast<size_t>(i)] - effectiveBwHz);
      if (diff < minDiff) {
        minDiff = diff;
        selected = i;
      }
    }
  }

  if (selected == m_bandwidthMode) {
    return;
  }
  m_bandwidthMode = selected;
  const int selectedBwHz = kXdrFmBwHz[static_cast<size_t>(selected)];
  // Parameterize liquid LPF from the mapped RF bandwidth:
  // channel BW -> baseband half-band cutoff, clamped to available Nyquist
  // headroom.
  const double nyquistHeadroomHz = 0.45 * static_cast<double>(m_inputRate);
  const double iqCutoffHz =
      (selectedBwHz > 0) ? std::clamp(static_cast<double>(selectedBwHz) * 0.5,
                                      9000.0, nyquistHeadroomHz)
                         : nyquistHeadroomHz;
  const float cutoffNorm = std::clamp(
      static_cast<float>(iqCutoffHz / static_cast<double>(m_inputRate)), 0.01f,
      0.45f);
  const std::uint32_t filterLen =
      (selectedBwHz > 0 && selectedBwHz <= 73000) ? kIqFirMaxLength : 81U;
  const float stopBandAtten =
      (selectedBwHz > 0 && selectedBwHz <= 42000) ? 70.0f : 60.0f;
  // Only the channel filter changes. Resetting the whole demodulator here
  // (and, in DspPipeline, the stereo decoder) dropped stereo for ~200 ms and
  // dipped the programme level by 5 dB on every W command and on every
  // adaptive-bandwidth step.
  designIqFilter(filterLen, cutoffNorm, stopBandAtten);
}

void FMDemod::designIqFilter(std::uint32_t length, float cutoffNorm,
                             float stopBandAtten) {
  m_liquidIqFilter.init(length, cutoffNorm, stopBandAtten, 0.0f,
                        m_iqFirL1Normalize);
  // The L1 bound lowers the filter's passband gain by 3-7 dB, depending on
  // the cutoff. The channel power drives the meter, the squelch and the
  // fade-mute, so it is measured as if the filter had unity gain.
  const double gain = std::max(1e-6, static_cast<double>(m_liquidIqFilter.dcGain()));
  m_iqFirPowerCorrection = 1.0 / (gain * gain);
  m_iqFirDelay = (kIqFirMaxLength - std::min(length, kIqFirMaxLength)) / 2;
  // Prime the new filter with the samples it would hold had it been running
  // all along: the last `length` inputs, each delayed by m_iqFirDelay.
  const std::size_t newest = m_iqHistoryPos + kIqHistorySize - 1;
  for (std::size_t j = length; j-- > 0;) {
    m_liquidIqFilter.push(
        m_iqHistory[(newest - m_iqFirDelay - j) % kIqHistorySize]);
  }
}

std::complex<float> FMDemod::filterIq(std::complex<float> sample) {
  m_iqHistory[m_iqHistoryPos] = sample;
  const std::complex<float> delayed =
      m_iqHistory[(m_iqHistoryPos + kIqHistorySize - m_iqFirDelay) %
                  kIqHistorySize];
  m_iqHistoryPos = (m_iqHistoryPos + 1) % kIqHistorySize;
  m_liquidIqFilter.push(delayed);
  return m_liquidIqFilter.execute();
}

void FMDemod::setIqFirL1Normalize(bool enabled) {
  if (enabled == m_iqFirL1Normalize) {
    return;
  }
  m_iqFirL1Normalize = enabled;
  // Re-design the IQ FIR with the current bandwidth + new normalization flag.
  // Forcing a re-init by bumping the mode through a sentinel keeps the
  // setBandwidthHz path canonical (it owns the cutoff/taps math).
  const int currentBw = (m_bandwidthMode >= 0 &&
                         m_bandwidthMode < static_cast<int>(kXdrFmBwHz.size()))
                            ? kXdrFmBwHz[static_cast<size_t>(m_bandwidthMode)]
                            : 0;
  m_bandwidthMode = -1;
  setBandwidthHz(currentBw > 0 ? currentBw : m_w0BandwidthHz);
}

void FMDemod::setW0BandwidthHz(int bwHz) {
  m_w0BandwidthHz = std::clamp(bwHz, 0, 400000);
}

void FMDemod::setMultipathEqMode(fm_tuner::dsp::MultipathEqMode mode,
                                 std::uint32_t taps) {
  m_liquidMultipathEq.init(mode, taps, static_cast<float>(m_inputRate));
}

void FMDemod::setDspAgcMode(DspAgcMode mode) {
  if (mode == m_dspAgcMode) {
    return;
  }
  m_dspAgcMode = mode;
  if (m_dspAgcMode != DspAgcMode::Off) {
    const float bandwidth =
        (m_dspAgcMode == DspAgcMode::Fast) ? 0.01f : 0.001f;
    m_liquidIqAgc.init(bandwidth, 1.0f);
  }
  reset();
}

void FMDemod::demodulate(const uint8_t *iq, float *audio, size_t len) {
  const auto &kIqNorm = iqNormLut();
  const uint8_t *iqPtr = iq;
  size_t clipCount = 0;
  double powerSum = 0.0;

  for (size_t i = 0; i < len; i++) {
    const uint8_t iByte = iqPtr[0];
    const uint8_t qByte = iqPtr[1];
    iqPtr += 2;
    if (fm_tuner::dsp::isRtlSdrIqByteSaturated(iByte) ||
        fm_tuner::dsp::isRtlSdrIqByteSaturated(qByte)) {
      clipCount++;
    }

    const std::complex<float> iqFiltered = filterIq(
        removeIqDc(std::complex<float>(kIqNorm[iByte], kIqNorm[qByte])));
    powerSum += static_cast<double>(iqFiltered.real()) * iqFiltered.real() +
                static_cast<double>(iqFiltered.imag()) * iqFiltered.imag();
    std::complex<float> iqDemodIn = iqFiltered;
    if (m_dspAgcMode != DspAgcMode::Off) {
      iqDemodIn = m_liquidIqAgc.execute(iqDemodIn);
    }
    // Multipath equalizer (CMA). No-op when mode == Off. Sits *after* the
    // channel FIR and IF AGC so the CMA sees a normalized envelope.
    iqDemodIn = m_liquidMultipathEq.execute(iqDemodIn);
    audio[i] = m_liquidFreqDemod.execute(iqDemodIn);
  }

  m_clipping = (clipCount > 0);
  m_clippingRatio =
      (len > 0) ? (static_cast<float>(clipCount) / static_cast<float>(len))
                : 0.0f;
  // Cap at 0 dBFS so ADC-rail overload (IQ FIR rings past full scale under
  // heavy IF AGC) doesn't report physically nonsensical positive dBFS.
  m_filteredChannelPowerDbfs =
      (len > 0)
          ? std::min(0.0,
                     10.0 *
                         std::log10((powerSum * m_iqFirPowerCorrection /
                                     static_cast<double>(len)) +
                                    1e-20))
          : -120.0;
}

void FMDemod::demodulateComplex(const std::complex<float> *iq, float *audio,
                                size_t len) {
  size_t clipCount = 0;
  double powerSum = 0.0;

  for (size_t i = 0; i < len; i++) {
    const float iRaw = iq[i].real();
    const float qRaw = iq[i].imag();
    if (std::abs(iRaw) >= 0.995f || std::abs(qRaw) >= 0.995f) {
      clipCount++;
    }

    const std::complex<float> iqFiltered = filterIq(removeIqDc(iq[i]));
    powerSum += static_cast<double>(iqFiltered.real()) * iqFiltered.real() +
                static_cast<double>(iqFiltered.imag()) * iqFiltered.imag();
    std::complex<float> iqDemodIn = iqFiltered;
    if (m_dspAgcMode != DspAgcMode::Off) {
      iqDemodIn = m_liquidIqAgc.execute(iqDemodIn);
    }
    iqDemodIn = m_liquidMultipathEq.execute(iqDemodIn);
    audio[i] = m_liquidFreqDemod.execute(iqDemodIn);
  }

  m_clipping = (clipCount > 0);
  m_clippingRatio =
      (len > 0) ? (static_cast<float>(clipCount) / static_cast<float>(len))
                : 0.0f;
  m_filteredChannelPowerDbfs =
      (len > 0)
          ? std::min(0.0,
                     10.0 *
                         std::log10((powerSum * m_iqFirPowerCorrection /
                                     static_cast<double>(len)) +
                                    1e-20))
          : -120.0;
}

void FMDemod::resetIqDc() {
  m_iqDcMean = {0.0, 0.0};
  m_iqDcAlpha = kIqDcAlphaFast;
}

std::complex<float> FMDemod::removeIqDc(std::complex<float> sample) {
  // The station is tuned to the centre of the band, so the front end's DC
  // offset sits on top of the carrier. A fixed 20 Hz DC blocker also removed
  // a carrier tuned within ~20 Hz of the centre: on a quiet passage an
  // unmodulated carrier became discriminator noise, and a modulated one was
  // distorted. The offset itself is static, so after a reset the tracker
  // starts at the old 20 Hz corner (the offset is gone within milliseconds
  // of a retune) and then glides to a 0.1 Hz corner in half a second, which
  // leaves a carrier even 2 Hz off centre untouched.
  const std::complex<double> x(sample.real(), sample.imag());
  m_iqDcMean += m_iqDcAlpha * (x - m_iqDcMean);
  if (m_iqDcAlpha > m_iqDcAlphaSlow) {
    m_iqDcAlpha = std::max(m_iqDcAlphaSlow, m_iqDcAlpha * m_iqDcAlphaDecay);
  }
  return {static_cast<float>(x.real() - m_iqDcMean.real()),
          static_cast<float>(x.imag() - m_iqDcMean.imag())};
}

float *FMDemod::ensureScratch(size_t numSamples) {
  if (m_demodScratch.size() < numSamples) {
    // Grow with headroom: a caller whose block size rises a little must not
    // reallocate (and copy) the whole buffer on every call. Reserving before
    // resizing also keeps the vector from moving its data twice.
    const size_t wanted = std::max(m_demodScratch.capacity() * 2, numSamples);
    m_demodScratch.reserve(wanted);
    m_demodScratch.resize(numSamples);
  }
  return m_demodScratch.data();
}

size_t FMDemod::downsampleAudio(const float *demod, float *audio,
                                size_t numSamples) {
  size_t outCount = 0;
  for (size_t i = 0; i < numSamples; i++) {
    const float in = m_deemphasisEnabled
                         ? m_liquidMonoDeemphasis.execute(demod[i])
                         : demod[i];
    const uint32_t produced =
        m_liquidMonoResampler.execute(in, m_liquidResampleTmp);
    for (uint32_t p = 0; p < produced; p++) {
      audio[outCount++] = m_liquidMonoDcBlock.execute(m_liquidResampleTmp[p]);
    }
  }
  return outCount;
}

void FMDemod::process(const uint8_t *iq, float *audio, size_t numSamples) {
  float *scratch = ensureScratch(numSamples);
  demodulate(iq, scratch, numSamples);
  downsampleAudio(scratch, audio, numSamples);
}

void FMDemod::processComplex(const std::complex<float> *iq, float *audio,
                             size_t numSamples) {
  float *scratch = ensureScratch(numSamples);
  demodulateComplex(iq, scratch, numSamples);
  downsampleAudio(scratch, audio, numSamples);
}

size_t FMDemod::processSplit(const uint8_t *iq, float *mpxOut, float *monoOut,
                             size_t numSamples) {
  float *scratch = ensureScratch(numSamples);
  demodulate(iq, scratch, numSamples);
  if (mpxOut) {
    std::memcpy(mpxOut, scratch, numSamples * sizeof(float));
  }
  if (!monoOut) {
    return 0;
  }
  return downsampleAudio(scratch, monoOut, numSamples);
}

size_t FMDemod::processSplitComplex(const std::complex<float> *iq,
                                    float *mpxOut, float *monoOut,
                                    size_t numSamples) {
  float *scratch = ensureScratch(numSamples);
  demodulateComplex(iq, scratch, numSamples);
  if (mpxOut) {
    std::memcpy(mpxOut, scratch, numSamples * sizeof(float));
  }
  if (!monoOut) {
    return 0;
  }
  return downsampleAudio(scratch, monoOut, numSamples);
}

void FMDemod::processNoDownsample(const uint8_t *iq, float *audio,
                                  size_t numSamples) {
  demodulate(iq, audio, numSamples);
}
