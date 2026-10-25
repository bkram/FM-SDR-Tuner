#ifndef FM_DEMOD_H
#define FM_DEMOD_H

#include "dsp/liquid_primitives.h"
#include "dsp/multipath_eq.h"
#include <array>
#include <complex>
#include <stddef.h>
#include <stdint.h>
#include <vector>

class FMDemod {
public:
  enum class DspAgcMode { Off = 0, Fast = 1, Slow = 2 };

  FMDemod(int inputRate, int outputRate);
  ~FMDemod();

  void process(const uint8_t *iq, float *audio, size_t numSamples);
  void processComplex(const std::complex<float> *iq, float *audio,
                      size_t numSamples);
  void processNoDownsample(const uint8_t *iq, float *audio, size_t numSamples);
  size_t processSplit(const uint8_t *iq, float *mpxOut, float *monoOut,
                      size_t numSamples);
  size_t processSplitComplex(const std::complex<float> *iq, float *mpxOut,
                             float *monoOut, size_t numSamples);
  size_t downsampleAudio(const float *demod, float *audio, size_t numSamples);
  void reset();

  void setDeemphasis(int tau_us);
  void setDeviation(double deviation);
  void setBandwidthMode(int mode);
  void setBandwidthHz(int bwHz);
  void setW0BandwidthHz(int bwHz);
  void setDspAgcMode(DspAgcMode mode);
  void setMultipathEqMode(fm_tuner::dsp::MultipathEqMode mode,
                          std::uint32_t taps = 17);
  void setMultipathAdaptEnabled(bool enabled) {
    m_liquidMultipathEq.setAdaptEnabled(enabled);
  }
  void setIqFirL1Normalize(bool enabled);
  float getMultipathEnvelopeError() const {
    return m_liquidMultipathEq.envelopeError();
  }
  bool isClipping() const { return m_clipping; }
  float getClippingRatio() const { return m_clippingRatio; }
  double getFilteredChannelPowerDbfs() const { return m_filteredChannelPowerDbfs; }

private:
  void demodulate(const uint8_t *iq, float *audio, size_t len);
  void demodulateComplex(const std::complex<float> *iq, float *audio,
                         size_t len);
  // Returns the demodulator's scratch buffer, grown to numSamples if needed.
  float *ensureScratch(size_t numSamples);
  // Removes the front end's static DC offset from one IQ sample.
  std::complex<float> removeIqDc(std::complex<float> sample);
  void resetIqDc();
  // Feeds one sample to the channel FIR through the IQ history (see
  // m_iqHistory) and returns the filtered sample.
  std::complex<float> filterIq(std::complex<float> sample);
  // Designs the channel FIR and primes it from the IQ history, so a
  // bandwidth change continues the stream instead of restarting it.
  void designIqFilter(std::uint32_t length, float cutoffNorm,
                      float stopBandAtten);

  int m_inputRate;
  int m_outputRate;

  double m_deviation;

  bool m_deemphasisEnabled;
  int m_bandwidthMode;
  int m_w0BandwidthHz;
  DspAgcMode m_dspAgcMode;
  bool m_iqFirL1Normalize = false;

  std::vector<float> m_demodScratch;

  bool m_clipping;
  float m_clippingRatio;
  double m_filteredChannelPowerDbfs;
  fm_tuner::dsp::liquid::FIRFilter m_liquidIqFilter;
  // The channel FIR's recent input. It lets a bandwidth change prime the new
  // filter with the samples the old one held, and it delays the input of a
  // shorter filter so every design has the longest one's group delay: the
  // discriminator output does not jump in time when the bandwidth changes.
  static constexpr std::size_t kIqHistorySize = 128;
  static constexpr std::uint32_t kIqFirMaxLength = 121;
  std::array<std::complex<float>, kIqHistorySize> m_iqHistory{};
  std::size_t m_iqHistoryPos = 0;
  std::size_t m_iqFirDelay = 0;
  fm_tuner::dsp::liquid::FreqDemod m_liquidFreqDemod;
  // IQ DC tracker: a running mean whose time constant starts short (20 Hz
  // corner) after a reset and glides to a 0.1 Hz corner. See removeIqDc().
  std::complex<double> m_iqDcMean{0.0, 0.0};
  double m_iqDcAlpha = 0.0;
  double m_iqDcAlphaSlow = 0.0;
  double m_iqDcAlphaDecay = 1.0;
  fm_tuner::dsp::liquid::IIRFilterReal m_liquidMonoDeemphasis;
  fm_tuner::dsp::liquid::IIRFilterReal m_liquidMonoDcBlock;
  fm_tuner::dsp::liquid::Resampler m_liquidMonoResampler;
  fm_tuner::dsp::liquid::AGC m_liquidIqAgc;
  fm_tuner::dsp::MultipathEqualizer m_liquidMultipathEq;
  std::array<float, fm_tuner::dsp::liquid::Resampler::kMaxOutput>
      m_liquidResampleTmp{};
};

#endif
