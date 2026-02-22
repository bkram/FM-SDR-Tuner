#include "dsp/liquid_primitives.h"

#include <stdexcept>

namespace fm_tuner::dsp::liquid {

AGC::~AGC() {
    if (m_object != nullptr) {
        agc_crcf_destroy(m_object);
    }
}

void AGC::init(float bandwidth, float initialGain) {
    if (m_object != nullptr) {
        agc_crcf_destroy(m_object);
    }
    m_object = agc_crcf_create();
    if (m_object == nullptr) {
        throw std::runtime_error("failed to create liquid agc_crcf");
    }
    m_bandwidth = bandwidth;
    m_initialGain = initialGain;
    agc_crcf_set_bandwidth(m_object, m_bandwidth);
    agc_crcf_set_gain(m_object, m_initialGain);
}

void AGC::reset() {
    if (m_object != nullptr) {
        agc_crcf_destroy(m_object);
        m_object = nullptr;
    }
    init(m_bandwidth, m_initialGain);
}

std::complex<float> AGC::execute(std::complex<float> sample) const {
    if (m_object == nullptr) {
        return sample;
    }
    std::complex<float> out{};
    agc_crcf_execute(m_object, sample, &out);
    return out;
}

FIRFilter::~FIRFilter() {
    if (m_object != nullptr) {
        firfilt_crcf_destroy(m_object);
    }
}

void FIRFilter::init(std::uint32_t length, float cutoff, float stopBandAtten, float center) {
    if (m_object != nullptr) {
        firfilt_crcf_destroy(m_object);
    }
    m_object = firfilt_crcf_create_kaiser(length, cutoff, stopBandAtten, center);
    if (m_object == nullptr) {
        throw std::runtime_error("failed to create liquid firfilt_crcf");
    }
    m_length = length;
    m_cutoff = cutoff;
    m_stopBandAtten = stopBandAtten;
    m_center = center;
    // For low-pass filters, normalize DC gain near unity.
    // For shifted filters (e.g. band-pass via non-zero center), keep native scaling.
    const float scale = (std::abs(m_center) < 1e-6f) ? (2.0f * m_cutoff) : 1.0f;
    firfilt_crcf_set_scale(m_object, scale);
}

void FIRFilter::reset() {
    if (m_object != nullptr) {
        firfilt_crcf_destroy(m_object);
        m_object = nullptr;
    }
    init(m_length, m_cutoff, m_stopBandAtten, m_center);
}

void FIRFilter::push(std::complex<float> sample) const {
    if (m_object != nullptr) {
        firfilt_crcf_push(m_object, sample);
    }
}

std::complex<float> FIRFilter::execute() const {
    if (m_object == nullptr) {
        return {};
    }
    std::complex<float> out{};
    firfilt_crcf_execute(m_object, &out);
    return out;
}

std::size_t FIRFilter::length() const {
    if (m_object == nullptr) {
        return 0;
    }
    return firfilt_crcf_get_length(m_object);
}

NCO::~NCO() {
    if (m_object != nullptr) {
        nco_crcf_destroy(m_object);
    }
}

void NCO::init(liquid_ncotype type, float angularFrequency) {
    if (m_object != nullptr) {
        nco_crcf_destroy(m_object);
    }
    m_object = nco_crcf_create(type);
    if (m_object == nullptr) {
        throw std::runtime_error("failed to create liquid nco_crcf");
    }
    m_type = type;
    m_angularFrequency = angularFrequency;
    nco_crcf_set_frequency(m_object, m_angularFrequency);
}

void NCO::reset() {
    if (m_object != nullptr) {
        nco_crcf_reset(m_object);
        nco_crcf_set_frequency(m_object, m_angularFrequency);
    }
}

void NCO::step() const {
    if (m_object != nullptr) {
        nco_crcf_step(m_object);
    }
}

void NCO::setPLLBandwidth(float bandwidth) const {
    if (m_object != nullptr) {
        nco_crcf_pll_set_bandwidth(m_object, bandwidth);
    }
}

void NCO::stepPLL(float phaseError) const {
    if (m_object != nullptr) {
        nco_crcf_pll_step(m_object, phaseError);
    }
}

float NCO::phase() const {
    if (m_object == nullptr) {
        return 0.0f;
    }
    return nco_crcf_get_phase(m_object);
}

Resampler::~Resampler() {
    if (m_object != nullptr) {
        resamp_rrrf_destroy(m_object);
    }
}

void Resampler::init(float ratio, std::uint32_t halfLength, float cutoff,
                     float stopBandAtten, std::uint32_t polyphaseFilters) {
    if (ratio < 0.005f || ratio > static_cast<float>(kMaxOutput)) {
        throw std::runtime_error("resampler ratio is out of supported range");
    }
    if (m_object != nullptr) {
        resamp_rrrf_destroy(m_object);
    }
    m_ratio = ratio;
    m_halfLength = halfLength;
    m_cutoff = cutoff;
    m_stopBandAtten = stopBandAtten;
    m_polyphaseFilters = polyphaseFilters;
    m_object = resamp_rrrf_create(m_ratio, m_halfLength, m_cutoff, m_stopBandAtten, m_polyphaseFilters);
    if (m_object == nullptr) {
        throw std::runtime_error("failed to create liquid resamp_rrrf");
    }
}

void Resampler::reset() {
    if (m_object != nullptr) {
        resamp_rrrf_destroy(m_object);
        m_object = nullptr;
    }
    init(m_ratio, m_halfLength, m_cutoff, m_stopBandAtten, m_polyphaseFilters);
}

std::uint32_t Resampler::execute(float input, std::array<float, kMaxOutput>& output) const {
    if (m_object == nullptr) {
        return 0;
    }
    unsigned written = 0;
    resamp_rrrf_execute(m_object, input, output.data(), &written);
    if (written > kMaxOutput) {
        return static_cast<std::uint32_t>(kMaxOutput);
    }
    return static_cast<std::uint32_t>(written);
}

}  // namespace fm_tuner::dsp::liquid
