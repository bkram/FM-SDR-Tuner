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
  fm_tuner::dsp::liquid::FreqDemod m_liquidFreqDemod;
  fm_tuner::dsp::liquid::IIRFilterReal m_liquidIqDcBlockI;
  fm_tuner::dsp::liquid::IIRFilterReal m_liquidIqDcBlockQ;
  fm_tuner::dsp::liquid::IIRFilterReal m_liquidMonoDeemphasis;
  fm_tuner::dsp::liquid::IIRFilterReal m_liquidMonoDcBlock;
  fm_tuner::dsp::liquid::Resampler m_liquidMonoResampler;
  fm_tuner::dsp::liquid::AGC m_liquidIqAgc;
  fm_tuner::dsp::MultipathEqualizer m_liquidMultipathEq;
  std::array<float, fm_tuner::dsp::liquid::Resampler::kMaxOutput>
      m_liquidResampleTmp{};
};

#endif
