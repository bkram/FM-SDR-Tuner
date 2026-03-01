#ifndef RUNTIME_LOOP_H
#define RUNTIME_LOOP_H

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>

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
    std::atomic<bool> &pendingBandwidth, int &appliedBandwidthHz,
    bool rtlConnected, bool verboseLogging, AudioOutput &audioOut,
    fm_tuner::dsp::Runtime &dspRuntime, RdsWorker &rdsWorker,
    DspPipeline &dspPipeline, size_t kRetuneMuteSamples,
    size_t &retuneMuteSamplesRemaining, size_t &retuneMuteTotalSamples,
    const std::function<void(const char *reason)> &applyRtlGainAndAgc,
    const std::function<void(uint32_t)> &tunerSetFrequency,
    const std::function<size_t(uint8_t *, size_t)> &tunerReadIQ,
    const std::function<void(const uint8_t *, size_t)> &writeIqCapture,
    const std::chrono::milliseconds &scanRetrySleep, uint8_t *iqBuffer,
    size_t sdrBufSamples, uint32_t iqSampleRate, int effectiveAppliedGainDb,
    double signalGainCompFactor, const Config::SDRSection &sdrConfig,
    const std::function<void(uint32_t, int)> &restoreAfterScan,
    const std::function<bool()> &shouldRun);

void maybeAdjustAutoGain(
    bool useSdrppGainStrategy, int cliGain, bool imsAgcEnabled,
    std::atomic<int> &requestedAGCMode, std::atomic<bool> &pendingAGC,
    std::chrono::steady_clock::time_point &lastGainDown,
    std::chrono::steady_clock::time_point &lastGainUp,
    const SignalLevelResult &signal, double clipRatio, float rfLevelFiltered,
    bool verboseLogging);

} // namespace runtime_loop

#endif
