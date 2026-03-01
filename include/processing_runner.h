#ifndef PROCESSING_RUNNER_H
#define PROCESSING_RUNNER_H

#include <cstddef>
#include <cstdint>
#include <functional>

#include "audio_output.h"
#include "config.h"
#include "dsp_pipeline.h"
#include "rds_worker.h"
#include "signal_level.h"
#include "xdr_server.h"

namespace processing_runner {

bool processAudioBlock(
    const uint8_t *iqBuffer, size_t samples, int outputRate,
    int effectiveAppliedGainDb, double signalGainCompFactor,
    const Config &config, bool verboseLogging,
    SignalLevelSmoother &rfLevelSmoother,
    const std::function<void(const SignalLevelResult &, double, float)>
        &autoGainHook,
    bool targetForceMono, bool &appliedEffectiveForceMono, DspPipeline &dspPipeline,
    RdsWorker &rdsWorker, XDRServer &xdrServer,
    size_t &retuneMuteSamplesRemaining, size_t &retuneMuteTotalSamples,
    AudioOutput &audioOut);

} // namespace processing_runner

#endif
