#ifndef RUNTIME_LOOP_H
#define RUNTIME_LOOP_H

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>

#include "adaptive_bandwidth.h"
#include "audio_output.h"
#include "config.h"
#include "dsp/runtime.h"
#include "dsp_pipeline.h"
#include "rds_worker.h"
#include "scan_engine.h"
#include "signal_level.h"
#include "xdr_server.h"

namespace runtime_loop {

bool handleControlAndScan(
    ScanEngine &scanEngine, XDRServer &xdrServer,
    std::atomic<uint32_t> &requestedFrequencyHz,
    std::atomic<bool> &pendingFrequency, std::atomic<bool> &pendingGain,
    std::atomic<bool> &pendingAGC, std::atomic<int> &requestedBandwidthHz,
    std::atomic<bool> &pendingBandwidth, std::atomic<int> &appliedBandwidthHz,
    bool rtlConnected, bool verboseLogging, AudioOutput &audioOut,
    fm_tuner::dsp::Runtime &dspRuntime, RdsWorker &rdsWorker,
    DspPipeline &dspPipeline, size_t kRetuneMuteSamples,
    size_t &retuneMuteSamplesRemaining, size_t &retuneMuteTotalSamples,
    const std::function<void(const char *reason)> &applyRtlGainAndAgc,
    const std::function<bool(uint32_t)> &tunerSetFrequency,
    const std::function<uint32_t()> &currentTunerFrequencyHz,
    const std::function<size_t(uint8_t *, size_t)> &tunerReadIQ,
    const std::function<void(const uint8_t *, size_t)> &writeIqCapture,
    const std::chrono::milliseconds &scanRetrySleep, uint8_t *iqBuffer,
    size_t sdrBufSamples, uint32_t iqSampleRate, int effectiveAppliedGainDb,
    double signalGainCompFactor, const Config::SDRSection &sdrConfig,
    const std::function<void(uint32_t, int)> &restoreAfterScan,
    const std::function<bool()> &shouldRun,
    const std::function<uint32_t(bool)> &setScanWideMode, bool &scanWideActive,
    uint32_t &scanWideRate, const std::function<void()> &tunerFlush = {});

// Advances the TEF auto-gain ladder (A0..A3) that protects the front end
// from overload. The ladder only runs when the gain is strategy-managed
// (gain_strategy=tef with rtl_gain_db=-1 and the tuner AGC off). On the
// paths where the gain is pinned -- strategy=sdrpp, a fixed command-line
// gain, or the tuner AGC -- `fixedGainAdvice` names the knob that controls
// the gain; when the front end is clipping on such a path the advice is
// printed, because an overloaded front end mixes a strong station with
// itself and the IMD product is heard as a duplicate of that station.
void maybeAdjustAutoGain(
    bool useSdrppGainStrategy, int cliGain, bool imsAgcEnabled,
    std::atomic<int> &requestedAGCMode, std::atomic<bool> &pendingAGC,
    std::chrono::steady_clock::time_point &lastGainDown,
    std::chrono::steady_clock::time_point &lastGainUp,
    const SignalLevelResult &signal, double clipRatio, float rfLevelFiltered,
    bool verboseLogging,
    const std::function<int(int)> &agcModeToGainDb,
    const char *fixedGainAdvice = nullptr);

// Honors processing.adaptive_bandwidth. Reads SNR from `signal`, picks a
// channel bandwidth via the policy in adaptive_bandwidth.h, and requests it
// via the (requestedBandwidthHz, pendingBandwidth) atomics so runtime_loop's
// existing bandwidth path applies the change. No-op if mode == Off.
void maybeAdjustAdaptiveBandwidth(
    fm_tuner::AdaptiveBandwidthMode mode,
    fm_tuner::AdaptiveBandwidthState &state,
    std::atomic<int> &requestedBandwidthHz, std::atomic<bool> &pendingBandwidth,
    int currentAppliedBandwidthHz, const SignalLevelResult &signal,
    bool verboseLogging);

} // namespace runtime_loop

#endif
