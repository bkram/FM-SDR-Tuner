#ifndef DSP_LIQUID_PRIMITIVES_H
#define DSP_LIQUID_PRIMITIVES_H

#include <array>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <vector>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#pragma clang diagnostic ignored "-Wreturn-type-c-linkage"
extern "C" {
#include "liquid/liquid.h"
}
#pragma clang diagnostic pop

namespace fm_tuner::dsp::liquid {

class AGC {
public:
  AGC() = default;
  ~AGC();
  AGC(const AGC &) = delete;
  AGC &operator=(const AGC &) = delete;
  AGC(AGC &&) = delete;
  AGC &operator=(AGC &&) = delete;

  void init(float bandwidth, float initialGain);
  void reset();
  std::complex<float> execute(std::complex<float> sample) const;
  bool ready() const { return m_object != nullptr; }

private:
  agc_crcf m_object = nullptr;
  float m_bandwidth = 0.0f;
  float m_initialGain = 1.0f;
};

class FIRFilter {
public:
  FIRFilter() = default;
  ~FIRFilter();
  FIRFilter(const FIRFilter &) = delete;
  FIRFilter &operator=(const FIRFilter &) = delete;
  FIRFilter(FIRFilter &&) = delete;
  FIRFilter &operator=(FIRFilter &&) = delete;

  // l1Normalize: when true and the filter is lowpass (center == 0), the
  // designed taps are explicitly L1-normalized — i.e., sum(|tap|) ≤ 1.
  // This guarantees that |y[n]| ≤ max|x[n]| at every output sample (the
  // classic mathematical bound: |y| = |Σ h·x| ≤ Σ|h|·max|x| ≤ max|x|).
  // Without this, a non-monotonic Kaiser FIR can ring past unit envelope
  // under ADC-rail saturation, producing post-filter samples whose power
  // exceeds the input dBFS — confusing the meter and gain policy. The
  // bandpass-centered path is always L1-normalized as a side-effect of
  // the centering step and ignores this flag.
  void init(std::uint32_t length, float cutoff, float stopBandAtten = 60.0f,
            float center = 0.0f, bool l1Normalize = false);
  void initWithTaps(const std::vector<float> &taps, float scale = 1.0f);
  void reset();
  void push(std::complex<float> sample) const;
  std::complex<float> execute() const;
  std::size_t length() const;
  bool ready() const { return m_object != nullptr; }

private:
  firfilt_crcf m_object = nullptr;
  std::uint32_t m_length = 0;
  float m_cutoff = 0.0f;
  float m_stopBandAtten = 60.0f;
  float m_center = 0.0f;
  bool m_l1Normalize = false;
  std::vector<float> m_taps{};
  bool m_useDirectTaps = false;
  float m_scale = 1.0f;
};

class NCO {
public:
  NCO() = default;
  ~NCO();
  NCO(const NCO &) = delete;
  NCO &operator=(const NCO &) = delete;
  NCO(NCO &&) = delete;
  NCO &operator=(NCO &&) = delete;

  void init(liquid_ncotype type, float angularFrequency);
  void reset();
  void step() const;
  void setPLLBandwidth(float bandwidth) const;
  void stepPLL(float phaseError) const;
  float phase() const;
  bool ready() const { return m_object != nullptr; }

private:
  nco_crcf m_object = nullptr;
  liquid_ncotype m_type = LIQUID_VCO;
  float m_angularFrequency = 0.0f;
};

class FreqDemod {
public:
  FreqDemod() = default;
  ~FreqDemod();
  FreqDemod(const FreqDemod &) = delete;
  FreqDemod &operator=(const FreqDemod &) = delete;
  FreqDemod(FreqDemod &&) = delete;
  FreqDemod &operator=(FreqDemod &&) = delete;

  void init(float modulationFactor);
  void reset();
  float execute(std::complex<float> sample) const;
  bool ready() const { return m_object != nullptr; }

private:
  freqdem m_object = nullptr;
  float m_modulationFactor = 1.0f;
};

class IIRFilterReal {
public:
  IIRFilterReal() = default;
  ~IIRFilterReal();
  IIRFilterReal(const IIRFilterReal &) = delete;
  IIRFilterReal &operator=(const IIRFilterReal &) = delete;
  IIRFilterReal(IIRFilterReal &&) = delete;
  IIRFilterReal &operator=(IIRFilterReal &&) = delete;

  void init(const std::vector<float> &b, const std::vector<float> &a);
  void initDCBlocker(float alpha);
  // Butterworth high-pass via liquid's IIR prototype designer (second-order
  // sections). cutoffNorm is a fraction of the sample rate (f/Fs). Used for the
  // demod-domain hiss-noise estimator (energy above the FM multiplex).
  void initHighpass(unsigned int order, float cutoffNorm,
                    float stopBandAtten = 60.0f);
  void reset();
  float execute(float input) const;
  bool ready() const { return m_object != nullptr; }

private:
  iirfilt_rrrf m_object = nullptr;
  std::vector<float> m_b{};
  std::vector<float> m_a{};
  bool m_useDcBlocker = false;
  float m_dcAlpha = 0.0f;
  bool m_useHighpass = false;
  unsigned int m_hpOrder = 4;
  float m_hpCutoffNorm = 0.0f;
  float m_hpStopAtten = 60.0f;
};

class Resampler {
public:
  static constexpr std::size_t kMaxOutput = 8;

  Resampler() = default;
  ~Resampler();
  Resampler(const Resampler &) = delete;
  Resampler &operator=(const Resampler &) = delete;
  Resampler(Resampler &&) = delete;
  Resampler &operator=(Resampler &&) = delete;

  void init(float ratio, std::uint32_t halfLength = 12, float cutoff = 0.47f,
            float stopBandAtten = 60.0f, std::uint32_t polyphaseFilters = 32);
  void reset();
  std::uint32_t execute(float input,
                        std::array<float, kMaxOutput> &output) const;
  bool ready() const { return m_object != nullptr; }

private:
  resamp_rrrf m_object = nullptr;
  float m_ratio = 1.0f;
  std::uint32_t m_halfLength = 12;
  float m_cutoff = 0.47f;
  float m_stopBandAtten = 60.0f;
  std::uint32_t m_polyphaseFilters = 32;
};

class ComplexDecimator {
public:
  ComplexDecimator() = default;
  ~ComplexDecimator();
  ComplexDecimator(const ComplexDecimator &) = delete;
  ComplexDecimator &operator=(const ComplexDecimator &) = delete;
  ComplexDecimator(ComplexDecimator &&) = delete;
  ComplexDecimator &operator=(ComplexDecimator &&) = delete;

  void init(std::uint32_t factor, std::uint32_t tapsPerPhase = 12,
            float stopBandAtten = 70.0f);
  void reset();
  std::size_t execute(const uint8_t *iqIn, std::size_t inSamples,
                      uint8_t *iqOut, std::size_t outCapacity) const;
  std::size_t executeComplex(const uint8_t *iqIn, std::size_t inSamples,
                             std::complex<float> *iqOut,
                             std::size_t outCapacity) const;
  // Decimate an already-normalized complex<float> input (e.g. SDRplay's
  // 16-bit IQ scaled to ±1.0). Same response as executeComplex; only the input
  // conversion differs (none — the samples arrive normalized).
  std::size_t executeComplexFromComplex(const std::complex<float> *iqIn,
                                        std::size_t inSamples,
                                        std::complex<float> *iqOut,
                                        std::size_t outCapacity) const;
  bool ready() const { return m_object != nullptr || m_factor == 1; }
  std::uint32_t factor() const { return m_factor; }

private:
  firdecim_crcf m_object = nullptr;
  std::uint32_t m_factor = 1;
  std::uint32_t m_tapsPerPhase = 12;
  float m_stopBandAtten = 70.0f;
  std::vector<float> m_taps{};
  mutable std::vector<std::complex<float>> m_block{};
};

} // namespace fm_tuner::dsp::liquid

#endif
