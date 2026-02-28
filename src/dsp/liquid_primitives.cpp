#include "dsp/liquid_primitives.h"
#include <liquid/liquid.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>

#if defined(LIQUID_VERSION_MAJOR) && defined(LIQUID_VERSION_MINOR)
#define FM_TUNER_LIQUID_FIRDES_KAISER_RETURNS_VOID                                                   \
    ((LIQUID_VERSION_MAJOR > 1) || (LIQUID_VERSION_MAJOR == 1 && LIQUID_VERSION_MINOR >= 4))
#elif defined(LIQUID_VERSION_NUMBER)
#define FM_TUNER_LIQUID_FIRDES_KAISER_RETURNS_VOID (LIQUID_VERSION_NUMBER >= 1004000)
#else
#define FM_TUNER_LIQUID_FIRDES_KAISER_RETURNS_VOID 0
#endif

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
    m_length = length;
    m_cutoff = cutoff;
    m_stopBandAtten = stopBandAtten;
    m_center = center;
    m_useDirectTaps = false;

    if (std::abs(m_center) < 1e-6f) {
        m_object = firfilt_crcf_create_kaiser(length, cutoff, stopBandAtten, 0.0f);
        if (m_object == nullptr) {
            throw std::runtime_error("failed to create liquid firfilt_crcf");
        }
        m_taps.clear();
        m_scale = 2.0f * m_cutoff;
        firfilt_crcf_set_scale(m_object, m_scale);
        return;
    }

    m_taps.assign(length, 0.0f);
#if FM_TUNER_LIQUID_FIRDES_KAISER_RETURNS_VOID
    liquid_firdes_kaiser(length, cutoff, stopBandAtten, 0.0f, m_taps.data());
#else
    if (liquid_firdes_kaiser(length, cutoff, stopBandAtten, 0.0f, m_taps.data()) != LIQUID_OK) {
        throw std::runtime_error("failed to design liquid kaiser taps");
    }
#endif
    const int mid = static_cast<int>(length / 2);
    constexpr float kTwoPi = 6.28318530717958647692f;
    for (std::uint32_t n = 0; n < length; ++n) {
        const float phase = kTwoPi * m_center * static_cast<float>(static_cast<int>(n) - mid);
        m_taps[n] = 2.0f * m_taps[n] * std::cos(phase);
    }
    double sumAbs = 0.0;
    for (float tap : m_taps) {
        sumAbs += std::abs(tap);
    }
    if (sumAbs > 1e-12) {
        const float invSumAbs = static_cast<float>(1.0 / sumAbs);
        for (float& tap : m_taps) {
            tap *= invSumAbs;
        }
    }
    m_object = firfilt_crcf_create(m_taps.data(), length);
    if (m_object == nullptr) {
        throw std::runtime_error("failed to create liquid firfilt_crcf from shifted taps");
    }
    m_scale = 1.0f;
    firfilt_crcf_set_scale(m_object, m_scale);
}

void FIRFilter::initWithTaps(const std::vector<float>& taps, float scale) {
    if (taps.empty()) {
        throw std::runtime_error("cannot create fir filter with empty taps");
    }
    if (m_object != nullptr) {
        firfilt_crcf_destroy(m_object);
    }
    m_taps = taps;
    m_object = firfilt_crcf_create(m_taps.data(), static_cast<unsigned int>(m_taps.size()));
    if (m_object == nullptr) {
        throw std::runtime_error("failed to create liquid firfilt_crcf from taps");
    }
    m_length = static_cast<std::uint32_t>(m_taps.size());
    m_cutoff = 0.0f;
    m_stopBandAtten = 0.0f;
    m_center = 0.0f;
    m_useDirectTaps = true;
    m_scale = scale;
    firfilt_crcf_set_scale(m_object, m_scale);
}

void FIRFilter::reset() {
    if (m_object != nullptr) {
        firfilt_crcf_destroy(m_object);
        m_object = nullptr;
    }
    if (m_useDirectTaps) {
        initWithTaps(m_taps, m_scale);
        return;
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

FreqDemod::~FreqDemod() {
    if (m_object != nullptr) {
        freqdem_destroy(m_object);
    }
}

void FreqDemod::init(float modulationFactor) {
    if (m_object != nullptr) {
        freqdem_destroy(m_object);
    }
    m_modulationFactor = modulationFactor;
    m_object = freqdem_create(m_modulationFactor);
    if (m_object == nullptr) {
        throw std::runtime_error("failed to create liquid freqdem");
    }
}

void FreqDemod::reset() {
    if (m_object != nullptr) {
        freqdem_reset(m_object);
    }
}

float FreqDemod::execute(std::complex<float> sample) const {
    if (m_object == nullptr) {
        return 0.0f;
    }
    float out = 0.0f;
    freqdem_demodulate(m_object, sample, &out);
    return out;
}

IIRFilterReal::~IIRFilterReal() {
    if (m_object != nullptr) {
        iirfilt_rrrf_destroy(m_object);
    }
}

void IIRFilterReal::init(const std::vector<float>& b, const std::vector<float>& a) {
    if (b.empty() || a.empty()) {
        throw std::runtime_error("iir filter requires non-empty numerator and denominator");
    }
    if (m_object != nullptr) {
        iirfilt_rrrf_destroy(m_object);
    }
    m_b = b;
    m_a = a;
    m_useDcBlocker = false;
    m_object = iirfilt_rrrf_create(const_cast<float*>(m_b.data()),
                                   static_cast<unsigned int>(m_b.size()),
                                   const_cast<float*>(m_a.data()),
                                   static_cast<unsigned int>(m_a.size()));
    if (m_object == nullptr) {
        throw std::runtime_error("failed to create liquid iirfilt_rrrf");
    }
}

void IIRFilterReal::initDCBlocker(float alpha) {
    if (m_object != nullptr) {
        iirfilt_rrrf_destroy(m_object);
    }
    m_useDcBlocker = true;
    m_dcAlpha = alpha;
    m_b.clear();
    m_a.clear();
    m_object = iirfilt_rrrf_create_dc_blocker(m_dcAlpha);
    if (m_object == nullptr) {
        throw std::runtime_error("failed to create liquid iirfilt_rrrf dc blocker");
    }
}

void IIRFilterReal::reset() {
    if (m_object != nullptr) {
        iirfilt_rrrf_destroy(m_object);
        m_object = nullptr;
    }
    if (m_useDcBlocker) {
        if (m_dcAlpha <= 0.0f) {
            return;
        }
        initDCBlocker(m_dcAlpha);
    } else {
        if (m_b.empty() || m_a.empty()) {
            return;
        }
        init(m_b, m_a);
    }
}

float IIRFilterReal::execute(float input) const {
    if (m_object == nullptr) {
        return input;
    }
    float out = 0.0f;
    iirfilt_rrrf_execute(m_object, input, &out);
    return out;
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

ComplexDecimator::~ComplexDecimator() {
    if (m_object != nullptr) {
        firdecim_crcf_destroy(m_object);
    }
}

void ComplexDecimator::init(std::uint32_t factor, std::uint32_t tapsPerPhase, float stopBandAtten) {
    if (factor == 0) {
        throw std::runtime_error("complex decimator factor must be >= 1");
    }
    if (m_object != nullptr) {
        firdecim_crcf_destroy(m_object);
        m_object = nullptr;
    }
    m_factor = factor;
    m_tapsPerPhase = std::max<std::uint32_t>(4, tapsPerPhase);
    m_stopBandAtten = stopBandAtten;
    m_block.assign(static_cast<std::size_t>(m_factor), std::complex<float>(0.0f, 0.0f));
    m_taps.clear();

    if (m_factor == 1) {
        return;
    }

    const std::uint32_t hLen = m_factor * m_tapsPerPhase;
    m_taps.assign(hLen, 0.0f);
    const float cutoff = std::clamp(0.45f / static_cast<float>(m_factor), 0.01f, 0.45f);
#if FM_TUNER_LIQUID_FIRDES_KAISER_RETURNS_VOID
    liquid_firdes_kaiser(hLen, cutoff, m_stopBandAtten, 0.0f, m_taps.data());
#else
    if (liquid_firdes_kaiser(hLen, cutoff, m_stopBandAtten, 0.0f, m_taps.data()) != LIQUID_OK) {
        throw std::runtime_error("failed to design complex decimator taps");
    }
#endif
    m_object = firdecim_crcf_create(m_factor, m_taps.data(), hLen);
    if (m_object == nullptr) {
        throw std::runtime_error("failed to create liquid firdecim_crcf");
    }
    firdecim_crcf_set_scale(m_object, 2.0f * cutoff);
}

void ComplexDecimator::reset() {
    if (m_factor == 1) {
        return;
    }
    if (m_object != nullptr) {
        firdecim_crcf_destroy(m_object);
        m_object = nullptr;
    }
    const std::uint32_t hLen = m_factor * m_tapsPerPhase;
    const float cutoff = std::clamp(0.45f / static_cast<float>(m_factor), 0.01f, 0.45f);
    m_object = firdecim_crcf_create(m_factor, m_taps.data(), hLen);
    if (m_object == nullptr) {
        throw std::runtime_error("failed to recreate liquid firdecim_crcf");
    }
    firdecim_crcf_set_scale(m_object, 2.0f * cutoff);
}

std::size_t ComplexDecimator::execute(const uint8_t* iqIn,
                                      std::size_t inSamples,
                                      uint8_t* iqOut,
                                      std::size_t outCapacity) const {
    if (!iqIn || !iqOut || inSamples == 0 || outCapacity == 0) {
        return 0;
    }

    if (m_factor == 1) {
        const std::size_t copySamples = std::min(inSamples, outCapacity);
        std::copy_n(iqIn, copySamples * 2, iqOut);
        return copySamples;
    }
    if (m_object == nullptr || m_block.size() != m_factor) {
        return 0;
    }

    static constexpr float kScale = 1.0f / 127.5f;
    const std::size_t blocks = std::min(inSamples / m_factor, outCapacity);
    for (std::size_t b = 0; b < blocks; b++) {
        const std::size_t inBase = b * m_factor;
        for (std::size_t k = 0; k < m_factor; k++) {
            const std::size_t idx = (inBase + k) * 2;
            const float i = (static_cast<float>(iqIn[idx]) - 127.5f) * kScale;
            const float q = (static_cast<float>(iqIn[idx + 1]) - 127.5f) * kScale;
            m_block[k] = std::complex<float>(i, q);
        }

        std::complex<float> y(0.0f, 0.0f);
        firdecim_crcf_execute(m_object, m_block.data(), &y);
        const float iOut = std::clamp((y.real() * 127.5f) + 127.5f, 0.0f, 255.0f);
        const float qOut = std::clamp((y.imag() * 127.5f) + 127.5f, 0.0f, 255.0f);
        const std::size_t outIdx = b * 2;
        iqOut[outIdx] = static_cast<uint8_t>(iOut);
        iqOut[outIdx + 1] = static_cast<uint8_t>(qOut);
    }
    return blocks;
}

std::size_t ComplexDecimator::executeComplex(const uint8_t* iqIn,
                                             std::size_t inSamples,
                                             std::complex<float>* iqOut,
                                             std::size_t outCapacity) const {
    if (!iqIn || !iqOut || inSamples == 0 || outCapacity == 0) {
        return 0;
    }

    static constexpr float kScale = 1.0f / 127.5f;
    if (m_factor == 1) {
        const std::size_t copySamples = std::min(inSamples, outCapacity);
        for (std::size_t i = 0; i < copySamples; i++) {
            const std::size_t idx = i * 2;
            const float iOut = (static_cast<float>(iqIn[idx]) - 127.5f) * kScale;
            const float qOut = (static_cast<float>(iqIn[idx + 1]) - 127.5f) * kScale;
            iqOut[i] = std::complex<float>(iOut, qOut);
        }
        return copySamples;
    }
    if (m_object == nullptr || m_block.size() != m_factor) {
        return 0;
    }

    const std::size_t blocks = std::min(inSamples / m_factor, outCapacity);
    for (std::size_t b = 0; b < blocks; b++) {
        const std::size_t inBase = b * m_factor;
        for (std::size_t k = 0; k < m_factor; k++) {
            const std::size_t idx = (inBase + k) * 2;
            const float i = (static_cast<float>(iqIn[idx]) - 127.5f) * kScale;
            const float q = (static_cast<float>(iqIn[idx + 1]) - 127.5f) * kScale;
            m_block[k] = std::complex<float>(i, q);
        }

        std::complex<float> y(0.0f, 0.0f);
        firdecim_crcf_execute(m_object, m_block.data(), &y);
        iqOut[b] = y;
    }
    return blocks;
}

}  // namespace fm_tuner::dsp::liquid
