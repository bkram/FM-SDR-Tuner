#ifndef DSP_LIQUID_PRIMITIVES_H
#define DSP_LIQUID_PRIMITIVES_H

#include <array>
#include <complex>
#include <cstddef>
#include <cstdint>

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
    AGC(const AGC&) = delete;
    AGC& operator=(const AGC&) = delete;
    AGC(AGC&&) = delete;
    AGC& operator=(AGC&&) = delete;

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
    FIRFilter(const FIRFilter&) = delete;
    FIRFilter& operator=(const FIRFilter&) = delete;
    FIRFilter(FIRFilter&&) = delete;
    FIRFilter& operator=(FIRFilter&&) = delete;

    void init(std::uint32_t length, float cutoff, float stopBandAtten = 60.0f, float center = 0.0f);
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
};

class NCO {
public:
    NCO() = default;
    ~NCO();
    NCO(const NCO&) = delete;
    NCO& operator=(const NCO&) = delete;
    NCO(NCO&&) = delete;
    NCO& operator=(NCO&&) = delete;

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

class Resampler {
public:
    static constexpr std::size_t kMaxOutput = 8;

    Resampler() = default;
    ~Resampler();
    Resampler(const Resampler&) = delete;
    Resampler& operator=(const Resampler&) = delete;
    Resampler(Resampler&&) = delete;
    Resampler& operator=(Resampler&&) = delete;

    void init(float ratio, std::uint32_t halfLength = 12, float cutoff = 0.47f,
              float stopBandAtten = 60.0f, std::uint32_t polyphaseFilters = 32);
    void reset();
    std::uint32_t execute(float input, std::array<float, kMaxOutput>& output) const;
    bool ready() const { return m_object != nullptr; }

private:
    resamp_rrrf m_object = nullptr;
    float m_ratio = 1.0f;
    std::uint32_t m_halfLength = 12;
    float m_cutoff = 0.47f;
    float m_stopBandAtten = 60.0f;
    std::uint32_t m_polyphaseFilters = 32;
};

}  // namespace fm_tuner::dsp::liquid

#endif
