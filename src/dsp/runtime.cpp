#include "dsp/runtime.h"

#include <iostream>
#include <stdexcept>

#include "dsp/liquid_primitives.h"

namespace fm_tuner::dsp {

struct Runtime::LiquidState {
    liquid::AGC agc;
    liquid::FIRFilter fir;
    liquid::NCO nco;
    liquid::Resampler resampler;

    void init() {
        agc.init(0.01f, 1.0f);
        fir.init(41, 0.12f);
        nco.init(LIQUID_VCO, 0.0f);
        nco.setPLLBandwidth(0.01f);
        resampler.init(1.0f);
    }

    void reset() {
        agc.reset();
        fir.reset();
        nco.reset();
        resampler.reset();
    }
};

const char* resetReasonName(ResetReason reason) {
    switch (reason) {
        case ResetReason::Start:
            return "start";
        case ResetReason::Stop:
            return "stop";
        case ResetReason::Retune:
            return "retune";
        case ResetReason::ScanRestore:
            return "scan_restore";
    }
    return "retune";
}

Runtime::Runtime(std::size_t blockSize, bool verbose)
    : m_blockSize(std::max<std::size_t>(1, blockSize))
    , m_verbose(verbose) {
    m_liquid = std::make_unique<LiquidState>();
    try {
        m_liquid->init();
        if (m_verbose) {
            std::cout << "[DSP] liquid primitives initialized\n";
        }
    } catch (const std::exception& ex) {
        throw std::runtime_error(std::string("failed to initialize liquid backend: ") + ex.what());
    }
}

Runtime::~Runtime() = default;

void Runtime::addResetHandler(std::function<void()> handler) {
    if (handler) {
        m_resetHandlers.push_back(std::move(handler));
    }
}

void Runtime::reset(ResetReason reason) const {
    if (m_verbose) {
        std::cout << "[DSP] reset reason=" << resetReasonName(reason) << "\n";
    }
    for (const auto& handler : m_resetHandlers) {
        handler();
    }
    if (m_liquid) {
        m_liquid->reset();
    }
}

}  // namespace fm_tuner::dsp
