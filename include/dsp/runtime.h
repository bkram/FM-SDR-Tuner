#ifndef DSP_RUNTIME_H
#define DSP_RUNTIME_H

#include <cstddef>
#include <functional>
#include <memory>
#include <vector>

namespace fm_tuner::dsp {

enum class ResetReason {
    Start = 0,
    Stop = 1,
    Retune = 2,
    ScanRestore = 3
};

const char* resetReasonName(ResetReason reason);

class Runtime {
public:
    Runtime(std::size_t blockSize, bool verbose);
    ~Runtime();

    std::size_t blockSize() const { return m_blockSize; }
    void addResetHandler(std::function<void()> handler);
    void reset(ResetReason reason) const;

private:
    struct LiquidState;
    std::size_t m_blockSize;
    bool m_verbose;
    std::vector<std::function<void()>> m_resetHandlers;
    std::unique_ptr<LiquidState> m_liquid;
};

}  // namespace fm_tuner::dsp

#endif
