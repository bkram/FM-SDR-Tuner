#include "application.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <thread>
#include <vector>

#include "audio_output.h"
#include "cpu_features.h"
#include "dsp/runtime.h"
#include "dsp_pipeline.h"
#include "mpx_audio_output.h"
#include "processing_runner.h"
#include "rds_worker.h"
#include "runtime_loop.h"
#include "scan_engine.h"
#include "signal_level.h"
#include "tuner_controller.h"
#include "tuner_session.h"
#include "wav_writer.h"
#include "xdr_facade.h"
#include "xdr_server.h"

namespace {

std::atomic<bool> g_running(true);

void signalHandler(int) { g_running = false; }

} // namespace

void Application::logStartup(const CPUFeatures &cpu) const {
  const Config &config = m_options.config;
  const bool verboseLogging = m_options.verboseLogging;
  if (!verboseLogging) {
    return;
  }
  std::cout << "[CPU] " << cpu.summary() << "\n";
  if (!m_options.configPath.empty()) {
    std::cout << "[Config] loaded: " << m_options.configPath << "\n";
  }
  std::cout << "[Config] audio.device='" << config.audio.device << "'\n";
  std::cout << "[Config] audio.startup_volume=" << config.audio.startup_volume
            << "\n";
  std::cout << "[Config] processing.dsp_block_samples="
            << config.processing.dsp_block_samples << "\n";
  std::cout << "[Config] processing.w0_bandwidth_hz="
            << config.processing.w0_bandwidth_hz << "\n";
  std::cout << "[Config] processing.dsp_agc='" << config.processing.dsp_agc
            << "'\n";
  std::cout << "[Config] processing.stereo_blend='"
            << config.processing.stereo_blend << "'\n";
  std::cout << "[Config] sdr.signal_bias_db=" << config.sdr.signal_bias_db
            << "\n";
  std::cout << "[Config] sdr.rtl_gain_db=" << config.sdr.rtl_gain_db << "\n";
  std::cout << "[Config] sdr.freq_correction_ppm="
            << config.sdr.freq_correction_ppm << "\n";
  std::cout << "[Config] sdr.low_latency_iq="
            << (config.sdr.low_latency_iq ? "true" : "false") << "\n";
  std::cout << "[Config] rtl_tcp.sample_rate=" << config.rtl_tcp.sample_rate
            << "\n";
}

bool Application::initAudioOutput(AudioOutput &audioOut,
                                  TunerSession &tunerSession,
                                  std::atomic<int> &requestedVolume) const {
  const Config &config = m_options.config;
  const bool verboseLogging = m_options.verboseLogging;
  if (verboseLogging) {
    std::cout << "[AUDIO] initializing audio output..." << "\n";
  }
  const std::string &audioDeviceToUse =
      !m_options.audioDevice.empty() ? m_options.audioDevice : config.audio.device;
  if (!audioOut.init(m_options.enableSpeaker, m_options.wavFile, audioDeviceToUse,
                     verboseLogging)) {
    std::cerr << "[AUDIO] failed to initialize audio output\n";
    tunerSession.disconnect();
    return false;
  }
  audioOut.setVolumePercent(requestedVolume.load());
  if (verboseLogging) {
    std::cout << "[AUDIO] audio output initialized" << "\n";
  }
  return true;
}

FILE *Application::openIqCapture(AudioOutput &audioOut,
                                 TunerSession &tunerSession) const {
  const bool verboseLogging = m_options.verboseLogging;
  if (m_options.iqFile.empty()) {
    return nullptr;
  }
  FILE *iqHandle = std::fopen(m_options.iqFile.c_str(), "wb");
  if (!iqHandle) {
    std::cerr << "[IQ] failed to open IQ output file: " << m_options.iqFile
              << "\n";
    audioOut.shutdown();
    tunerSession.disconnect();
    return nullptr;
  }
  if (verboseLogging) {
    std::cout << "[IQ] capture enabled: " << m_options.iqFile << "\n";
  }
  return iqHandle;
}

bool Application::initMpxCapture(WavWriter &mpxWavOut,
                                 TunerSession &tunerSession,
                                 uint32_t sampleRate) const {
  if (m_options.mpxWavFile.empty()) {
    return true;
  }
  // sampleRate is the rate at which MPX samples arrive from the DSP pipeline
  // (= post-decimation IQ rate). If the user asked for a different on-disk
  // rate we tell WavWriter to resample inline; otherwise the WAV is written
  // at the native MPX rate (current behaviour).
  const uint32_t targetRate = (m_options.mpxWavSampleRate != 0)
                                  ? m_options.mpxWavSampleRate
                                  : sampleRate;
  if (!mpxWavOut.init(m_options.mpxWavFile, targetRate, 1,
                      m_options.verboseLogging, "MPX WAV", sampleRate)) {
    std::cerr << "[MPX] failed to initialize MPX WAV output: "
              << m_options.mpxWavFile << "\n";
    tunerSession.disconnect();
    return false;
  }
  const float mpxGainLin = std::pow(10.0f, m_options.mpxGainDb / 20.0f);
  mpxWavOut.setGain(mpxGainLin);
  if (m_options.verboseLogging) {
    std::cout << "[MPX] capture enabled: " << m_options.mpxWavFile << " ("
              << targetRate << " Hz mono";
    if (targetRate != sampleRate) {
      std::cout << ", resampled from " << sampleRate << " Hz";
    }
    std::cout << ")\n";
    // Calibration line for downstream analyzers: the demod is 1.0 = 75 kHz
    // deviation, so after output gain, digital full scale = 75/gain kHz.
    std::cout << "[MPX] output gain " << m_options.mpxGainDb
              << " dB (full scale = " << (75.0f / mpxGainLin)
              << " kHz deviation)\n";
  }
  return true;
}

bool Application::initMpxAudio(MpxAudioOutput &mpxAudioOut,
                               TunerSession &tunerSession,
                               uint32_t sourceSampleRate) const {
  if (!m_options.mpxAudioEnabled) {
    return true;
  }

  // Collision guard: routing the 48 kHz stereo audio AND the live MPX to the
  // same audio device produces a mix that's useless for both paths (Core
  // Audio / ALSA will sum both streams onto the device). Best-effort check —
  // we compare the (case-insensitive, trimmed) selectors. If both end up
  // resolving to the same physical device we'd need OS-specific device-ID
  // lookups; the string check catches the common cases (same name, both
  // empty meaning system default).
  if (m_options.enableSpeaker) {
    auto normalize = [](std::string s) {
      while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
        s.erase(s.begin());
      }
      while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.pop_back();
      }
      std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
      });
      return s;
    };
    const std::string audioSel = normalize(
        !m_options.audioDevice.empty() ? m_options.audioDevice
                                       : m_options.config.audio.device);
    const std::string mpxSel = normalize(m_options.mpxAudioDevice);
    const bool bothDefault = audioSel.empty() && mpxSel.empty();
    const bool sameName = !audioSel.empty() && audioSel == mpxSel;
    if (bothDefault || sameName) {
      std::cerr
          << "[MPX-AUDIO] refusing to start: the 48 kHz audio output and the "
             "live MPX output are pointed at the same device "
          << (bothDefault ? "(both unset → system default)" :
                          ("('" + audioSel + "')"))
          << ". They would be summed on the device and the MPX would be "
             "corrupted. Pick a different --mpx-audio-device, or pass "
             "--no-audio to disable the 48 kHz audio output.\n";
      tunerSession.disconnect();
      return false;
    }
  }

  // Pick a target rate that preserves the full MPX spectrum (up to 75 kHz
  // RDS sideband). Defaults to 192 kHz unless --mpx-rate overrides — same
  // knob as the WAV path so the two sinks stay consistent.
  const uint32_t targetRate =
      (m_options.mpxWavSampleRate != 0) ? m_options.mpxWavSampleRate : 192000;
  if (!mpxAudioOut.init(m_options.mpxAudioDevice, sourceSampleRate, targetRate,
                        m_options.verboseLogging)) {
    std::cerr << "[MPX-AUDIO] failed to initialize live MPX audio output\n";
    tunerSession.disconnect();
    return false;
  }
  const float mpxGainLin = std::pow(10.0f, m_options.mpxGainDb / 20.0f);
  mpxAudioOut.setGain(mpxGainLin);
  if (m_options.verboseLogging) {
    std::cout << "[MPX-AUDIO] output gain " << m_options.mpxGainDb
              << " dB (full scale = " << (75.0f / mpxGainLin)
              << " kHz deviation)\n";
  }
  return true;
}

bool Application::readIqSamples(
    const std::function<size_t(uint8_t *, size_t)> &tunerReadIQ,
    const std::function<void(const uint8_t *, size_t)> &writeIqCapture,
    uint8_t *iqBuffer, size_t sdrBufSamples,
    const std::chrono::milliseconds &noDataSleep, TunerSession &tunerSession,
    bool verboseLogging, size_t &samples) const {
  samples = tunerReadIQ(iqBuffer, sdrBufSamples);
  if (samples == 0) {
    tunerSession.noteReadFailureAndMaybeReconnect();
    std::this_thread::sleep_for(noDataSleep);
    return false;
  }
  writeIqCapture(iqBuffer, samples);
  if (verboseLogging && samples < sdrBufSamples) {
    static uint32_t shortIqReadCount = 0;
    const uint32_t count = ++shortIqReadCount;
    if (count <= 5 || (count % 50) == 0) {
      std::cerr << "[SDR] short IQ read (" << count << "): " << samples << "/"
                << sdrBufSamples << " samples\n";
    }
  }
  tunerSession.resetReadFailures();
  return true;
}

void Application::shutdownResources(AudioOutput &audioOut, FILE *&iqHandle,
                                    WavWriter &mpxWavOut, XDRServer &xdrServer,
                                    TunerSession &tunerSession) const {
  audioOut.shutdown();
  mpxWavOut.shutdown();
  if (iqHandle) {
    std::fclose(iqHandle);
    iqHandle = nullptr;
  }
  xdrServer.stop();
  tunerSession.disconnect();
}

int Application::run() {
  constexpr int INPUT_RATE = 256000;
  constexpr int OUTPUT_RATE = 48000;

  const CPUFeatures cpu = detectCPUFeatures();

  const Config &config = m_options.config;
  const bool verboseLogging = m_options.verboseLogging;
  logStartup(cpu);
  std::string tcpHost = m_options.tcpHost;
  uint16_t tcpPort = m_options.tcpPort;
  uint32_t iqSampleRate = m_options.iqSampleRate;
  std::string tunerSource = m_options.tunerSource;
  uint32_t rtlDeviceIndex = m_options.rtlDeviceIndex;
  uint32_t freqKHz = m_options.freqKHz;
  int gain = m_options.gain;
  const int freqCorrectionPpm =
      std::clamp(config.sdr.freq_correction_ppm, -250, 250);
  const bool useSdrppGainStrategy = (config.sdr.gain_strategy == "sdrpp");
  const int defaultCustomGainFlags = config.sdr.default_custom_gain_flags;
  std::string xdrPassword = m_options.xdrPassword;
  bool xdrGuestMode = m_options.xdrGuestMode;
  if (xdrPassword.empty() && !xdrGuestMode) {
    xdrGuestMode = true;
    if (m_options.verboseLogging) {
      std::cout << "[XDR] no password set, enabling guest mode\n";
    }
  }
  uint16_t xdrPort = m_options.xdrPort;
  bool autoReconnect = m_options.autoReconnect;
  const bool autoStartTuner = m_options.autoStart;
  bool lowLatencyIq = m_options.lowLatencyIq;

  const size_t iqDecimation = static_cast<size_t>(iqSampleRate / INPUT_RATE);

  signal(SIGINT, signalHandler);
  signal(SIGTERM, signalHandler);

  TunerController tuner(tunerSource, tcpHost, tcpPort, rtlDeviceIndex);
  tuner.setLowLatencyMode(lowLatencyIq);
  const bool useDirectRtlSdr = tuner.isDirectRtlSdr();
  bool rtlConnected = false;
  XdrCommandState xdrState(freqKHz * 1000U, defaultCustomGainFlags,
                           std::clamp(config.processing.agc_mode, 0, 3), 0,
                           std::clamp(config.audio.startup_volume, 0, 100),
                           std::clamp(config.tuner.deemphasis, 0, 2), false);
  auto &requestedFrequencyHz = xdrState.requestedFrequencyHz;
  auto &requestedCustomGain = xdrState.requestedCustomGain;
  auto &requestedAGCMode = xdrState.requestedAGCMode;
  auto &requestedBandwidthHz = xdrState.requestedBandwidthHz;
  auto &requestedVolume = xdrState.requestedVolume;
  auto &requestedDeemphasis = xdrState.requestedDeemphasis;
  auto &requestedForceMono = xdrState.requestedForceMono;
  auto &requestedBlendMode = xdrState.requestedBlendMode;
  auto &pendingBlendMode = xdrState.pendingBlendMode;
  auto &pendingFrequency = xdrState.pendingFrequency;
  auto &pendingGain = xdrState.pendingGain;
  auto &pendingAGC = xdrState.pendingAGC;
  auto &pendingBandwidth = xdrState.pendingBandwidth;

  auto tunerName = [&]() -> const char * { return tuner.name(); };
  if (verboseLogging && useDirectRtlSdr) {
    std::cout << "[SDR] low-latency IQ mode: "
              << (lowLatencyIq ? "enabled" : "disabled") << "\n";
  }
  auto tunerSetFrequency = [&](uint32_t freqHz) -> bool {
    return tuner.setFrequency(freqHz);
  };
  auto tunerSetGainMode = [&](bool manual) -> bool {
    return tuner.setGainMode(manual);
  };
  auto tunerSetGain = [&](uint32_t gainTenthsDb) -> bool {
    return tuner.setGain(gainTenthsDb);
  };
  auto tunerSetAgc = [&](bool enable) -> bool { return tuner.setAGC(enable); };
  auto tunerReadIQ = [&](uint8_t *buffer, size_t maxSamples) -> size_t {
    return tuner.readIQ(buffer, maxSamples);
  };

  auto formatCustomGain = [&](int custom) -> std::string {
    const int rf = ((custom / 10) % 10) ? 1 : 0;
    const int ifv = (custom % 10) ? 1 : 0;
    char buffer[3];
    std::snprintf(buffer, sizeof(buffer), "%d%d", rf, ifv);
    return std::string(buffer);
  };

  auto isImsAgcEnabled = [&]() -> bool {
    if (gain >= 0) {
      return false;
    }
    const int custom = requestedCustomGain.load();
    const bool ims = (custom % 10) != 0;
    return ims;
  };

  auto calculateAppliedGainDb = [&]() -> int {
    const int agcMode = std::clamp(requestedAGCMode.load(), 0, 3);
    auto agcModeToGainDb = [&](int mode) -> int {
      const int clippedMode = std::clamp(mode, 0, 3);
      if (useDirectRtlSdr && !useSdrppGainStrategy) {
        static constexpr int kDirectRtlAutoGainDb[4] = {18, 12, 7, 0};
        return kDirectRtlAutoGainDb[clippedMode];
      }
      static constexpr int kTefGainDb[4] = {44, 36, 30, 24};
      return kTefGainDb[clippedMode];
    };
    int custom = requestedCustomGain.load();
    const bool ceq = ((custom / 10) % 10) != 0;
    int gainDb = agcModeToGainDb(agcMode) + (ceq ? 4 : 0);
    if (gain >= 0) {
      gainDb = gain;
    }
    return std::clamp(gainDb, 0, 49);
  };

  auto agcModeToGainDb = [&](int agcMode) -> int {
    const int clippedMode = std::clamp(agcMode, 0, 3);
    if (useDirectRtlSdr && !useSdrppGainStrategy) {
      static constexpr int kDirectRtlAutoGainDb[4] = {18, 12, 7, 0};
      return kDirectRtlAutoGainDb[clippedMode];
    }
    static constexpr int kTefGainDb[4] = {44, 36, 30, 24};
    return kTefGainDb[clippedMode];
  };

  auto effectiveAppliedGainDb = [&]() -> int {
    if (isImsAgcEnabled()) {
      return 0;
    }
    return calculateAppliedGainDb();
  };
  constexpr double kSignalGainCompFactor = 0.5;

  auto applyRtlGainAndAgc = [&](const char *reason) {
    if (!rtlConnected) {
      return;
    }
    if (useSdrppGainStrategy) {
      bool okGainMode = false;
      bool okAgc = false;
      bool okGain = true;
      if (gain >= 0) {
        okGainMode = tunerSetGainMode(true);
        okGain =
            tunerSetGain(static_cast<uint32_t>(std::clamp(gain, 0, 49) * 10));
      } else {
        okGainMode = tunerSetGainMode(true);
        okGain = tunerSetGain(
            static_cast<uint32_t>(config.sdr.sdrpp_rtl_agc_gain_db * 10));
      }
      okAgc = tunerSetAgc(config.sdr.sdrpp_rtl_agc);
      if (verboseLogging) {
        std::cout << "[SDR] " << reason << " strategy=sdrpp"
                  << " tuner_agc=" << ((gain < 0) ? 1 : 0)
                  << " rtl_agc=" << (config.sdr.sdrpp_rtl_agc ? 1 : 0)
                  << " if_gain="
                  << ((gain >= 0) ? std::clamp(gain, 0, 49)
                                  : config.sdr.sdrpp_rtl_agc_gain_db)
                  << " dB\n";
      }
      if (!okGainMode || !okAgc || !okGain) {
        std::cerr << "[SDR] warning: sdrpp gain/apply command failed"
                  << " setGainMode=" << (okGainMode ? 1 : 0)
                  << " setAGC=" << (okAgc ? 1 : 0)
                  << " setGain=" << (okGain ? 1 : 0) << "\n";
      }
      return;
    }
    const int agcMode = std::clamp(requestedAGCMode.load(), 0, 3);
    const int custom = requestedCustomGain.load();
    const int rf = ((custom / 10) % 10) ? 1 : 0;
    const int ifv = (custom % 10) ? 1 : 0;
    const bool imsAgc = isImsAgcEnabled();
    const int gainDb = calculateAppliedGainDb();

    bool okGainMode = false;
    bool okAgc = false;
    bool okGain = true;

    if (imsAgc) {
      okGainMode = tunerSetGainMode(false);
      okAgc = tunerSetAgc(true);
    } else {
      okGainMode = tunerSetGainMode(true);
      okAgc = tunerSetAgc(false);
      okGain = tunerSetGain(static_cast<uint32_t>(gainDb * 10));
    }

    if (verboseLogging) {
      std::cout << "[SDR] " << reason;
      if (gain >= 0) {
        std::cout << " fixed_gain=" << gain << " dB";
      } else {
        std::cout << " A" << agcMode << "(table=" << agcModeToGainDb(agcMode)
                  << " dB) G" << formatCustomGain(custom)
                  << "(flags rf=" << rf << ",if=" << ifv << ")";
      }
      std::cout
                << " -> mode=" << (imsAgc ? "auto" : "manual")
                << " tuner_gain=" << gainDb << " dB"
                << " manual=" << (imsAgc ? 0 : 1)
                << " rtl_agc=" << (imsAgc ? 1 : 0) << "\n";
    }
    if (!okGainMode || !okAgc || !okGain) {
      std::cerr << "[SDR] warning: gain/apply command failed"
                << " setGainMode=" << (okGainMode ? 1 : 0)
                << " setAGC=" << (okAgc ? 1 : 0)
                << " setGain=" << (okGain ? 1 : 0) << "\n";
    }
  };

  TunerSession tunerSession(
      tuner, rtlConnected,
      {.useDirectRtlSdr = useDirectRtlSdr,
       .verboseLogging = verboseLogging,
       .rtlDeviceIndex = rtlDeviceIndex,
       .tcpHost = tcpHost,
       .tcpPort = tcpPort,
       .initialFreqKHz = freqKHz,
       .iqSampleRate = iqSampleRate,
       .freqCorrectionPpm = freqCorrectionPpm,
       .autoReconnect = autoReconnect},
      [&]() { return requestedFrequencyHz.load(); },
      [&]() { return requestedAGCMode.load(); },
      [&]() { return requestedCustomGain.load(); },
      [&](const char *reason) { applyRtlGainAndAgc(reason); });

  if (verboseLogging) {
    std::cout << "[SDR] iq_sample_rate=" << iqSampleRate
              << " decimation=" << iqDecimation
              << " dsp_input_rate=" << INPUT_RATE << "\n";
  }
  const std::size_t dspBlockSize = static_cast<std::size_t>(
      std::clamp(config.processing.dsp_block_samples, 1024, 32768));
  fm_tuner::dsp::Runtime dspRuntime(dspBlockSize, verboseLogging);
  if (verboseLogging) {
    std::cout << "[DSP] block_samples=" << dspBlockSize << "\n";
  }
  DspPipeline dspPipeline(INPUT_RATE, OUTPUT_RATE, config.processing,
                          verboseLogging, dspBlockSize, iqDecimation);
  if (!m_options.stereoBlendOverride.empty()) {
    if (m_options.stereoBlendOverride == "soft") {
      dspPipeline.setBlendMode(StereoDecoder::BlendMode::Soft);
    } else if (m_options.stereoBlendOverride == "aggressive") {
      dspPipeline.setBlendMode(StereoDecoder::BlendMode::Aggressive);
    } else {
      dspPipeline.setBlendMode(StereoDecoder::BlendMode::Normal);
    }
    if (verboseLogging) {
      std::cout << "[DSP] stereo_blend override: "
                << m_options.stereoBlendOverride << "\n";
    }
  }
  dspRuntime.addResetHandler([&]() { dspPipeline.reset(); });
  int appliedBandwidthHz = requestedBandwidthHz.load();
  int appliedDeemphasis = requestedDeemphasis.load();
  bool appliedForceMono = requestedForceMono.load();
  bool appliedEffectiveForceMono = appliedForceMono;
  constexpr size_t kRetuneMuteSamples =
      static_cast<size_t>(OUTPUT_RATE / 25);
  // Cold-start settle takes longer than a retune (RF AGC converging from
  // scratch; glitches observed out to ~70 ms) and there is nothing to hear
  // yet, so the start mute can afford a longer window: 120 ms.
  constexpr size_t kStartMuteSamples =
      static_cast<size_t>((OUTPUT_RATE * 3) / 25);
  SignalLevelSmoother rfLevelSmoother;
  dspPipeline.setDeemphasisMode(appliedDeemphasis);
  dspPipeline.setForceMono(appliedEffectiveForceMono);
  dspPipeline.setBandwidthHz(appliedBandwidthHz);

  AudioOutput audioOut;
  WavWriter mpxWavOut;
  MpxAudioOutput mpxAudioOut;
  if (!initAudioOutput(audioOut, tunerSession, requestedVolume)) {
    return 1;
  }
  if (!initMpxCapture(mpxWavOut, tunerSession, INPUT_RATE)) {
    audioOut.shutdown();
    return 1;
  }
  if (!initMpxAudio(mpxAudioOut, tunerSession, INPUT_RATE)) {
    mpxWavOut.shutdown();
    audioOut.shutdown();
    return 1;
  }

  FILE *iqHandle = openIqCapture(audioOut, tunerSession);
  if (!m_options.iqFile.empty() && !iqHandle) {
    mpxAudioOut.shutdown();
    mpxWavOut.shutdown();
    return 1;
  }
  auto writeIqCapture = [&](const uint8_t *data, size_t sampleCount) {
    if (!iqHandle || !data || sampleCount == 0) {
      return;
    }
    (void)std::fwrite(data, 1, sampleCount * 2, iqHandle);
  };

  std::atomic<bool> tunerActive(false);
  std::atomic<bool> pendingStartRequest(false);
  std::atomic<bool> pendingStopRequest(false);

  XDRServer xdrServer(xdrPort);
  XdrFacade xdrFacade(xdrServer, xdrState,
                      {.verboseLogging = verboseLogging,
                       .useSdrppGainStrategy = useSdrppGainStrategy,
                       .allowClientGainOverride =
                           config.processing.client_gain_allowed,
                       .fixedLocalGain = (gain >= 0)});
  xdrFacade.configureServer(xdrPassword, xdrGuestMode);
  xdrFacade.installCallbacks(
      [&](int volumePercent) { audioOut.setVolumePercent(volumePercent); },
      [&]() {
        pendingStartRequest.store(true, std::memory_order_release);
      },
      [&]() {
        pendingStopRequest.store(true, std::memory_order_release);
      },
      formatCustomGain);

  if (!xdrServer.start()) {
    std::cerr << "[XDR] failed to start XDR server\n";
  }

  if (autoStartTuner) {
    if (verboseLogging) {
      std::cout << "[AUTO] auto-starting tuner for local mode\n";
    }
    tunerSession.connect();
    tunerActive = rtlConnected;
    if (!rtlConnected) {
      std::cerr << "[AUTO] warning: auto-start requested but " << tunerName()
                << " connect failed\n";
    }
  }

  RdsWorker rdsWorker(INPUT_RATE, [&](const RDSGroup &group) {
    xdrServer.updateRDS(group.blockA, group.blockB, group.blockC, group.blockD,
                        group.errors);
  });
  rdsWorker.start();

  if (verboseLogging) {
    std::cout << "[APP] waiting for client connection on port " << xdrPort
              << "...\n";
    std::cout << "[APP] press Ctrl+C to stop.\n";
  }

  const size_t SDR_BUF_SAMPLES = dspPipeline.sdrBlockSamples();
  const auto noDataSleep = useDirectRtlSdr ? std::chrono::milliseconds(2)
                                           : std::chrono::milliseconds(10);
  const auto scanRetrySleep = useDirectRtlSdr ? std::chrono::milliseconds(2)
                                              : std::chrono::milliseconds(5);
  std::vector<uint8_t> iqBufferStorage(SDR_BUF_SAMPLES * 2, 0);
  uint8_t *iqBuffer = iqBufferStorage.data();

  // Pre-armed so the very first processed samples after startup (including
  // the --auto-start path, which connects before this loop) get the same
  // settle mute as a retune. The counter only decrements while samples flow.
  size_t retuneMuteSamplesRemaining = kStartMuteSamples;
  size_t retuneMuteTotalSamples = kStartMuteSamples;

  ScanEngine scanEngine;
  auto lastGainDown =
      std::chrono::steady_clock::now() - std::chrono::seconds(5);
  auto lastGainUp = std::chrono::steady_clock::now() - std::chrono::seconds(5);
  fm_tuner::AdaptiveBandwidthState adaptiveBwState;
  fm_tuner::AdaptiveBandwidthMode adaptiveBwMode =
      fm_tuner::AdaptiveBandwidthMode::Off;
  {
    std::string raw = config.processing.adaptive_bandwidth;
    std::transform(raw.begin(), raw.end(), raw.begin(), [](unsigned char c) {
      return static_cast<char>(std::tolower(c));
    });
    if (raw == "conservative") {
      adaptiveBwMode = fm_tuner::AdaptiveBandwidthMode::Conservative;
    } else if (raw == "aggressive") {
      adaptiveBwMode = fm_tuner::AdaptiveBandwidthMode::Aggressive;
    }
  }
  auto restoreAfterScan = [&](uint32_t restoreFreqHz, int restoreBandwidthHz) {
    requestedBandwidthHz = restoreBandwidthHz;
    pendingBandwidth = true;
    requestedFrequencyHz.store(restoreFreqHz, std::memory_order_relaxed);
    pendingFrequency.store(true, std::memory_order_release);
    if (rtlConnected) {
      tunerSetFrequency(restoreFreqHz);
    }
    dspRuntime.reset(fm_tuner::dsp::ResetReason::ScanRestore);
    retuneMuteSamplesRemaining = kRetuneMuteSamples;
    retuneMuteTotalSamples = kRetuneMuteSamples;
    rdsWorker.requestReset();
  };
  while (g_running) {
    if (pendingStopRequest.exchange(false, std::memory_order_acq_rel)) {
      pendingStartRequest.store(false, std::memory_order_release);
      tunerActive.store(false, std::memory_order_release);
      dspRuntime.reset(fm_tuner::dsp::ResetReason::Stop);
      audioOut.clearRealtimeQueue();
      tunerSession.disconnect();
    }

    if (pendingStartRequest.exchange(false, std::memory_order_acq_rel)) {
      tunerSession.connect();
      dspRuntime.reset(fm_tuner::dsp::ResetReason::Start);
      // Arm the settle mute on start as well as on retune: the demod's
      // PLL/AGC settling emits a glitch burst (spikes to ~2x deviation full
      // scale) for the first tens of ms, which otherwise reaches the audio
      // AND the MPX outputs (WAV / live MPX / RDS) unmuted.
      retuneMuteSamplesRemaining = kStartMuteSamples;
      retuneMuteTotalSamples = kStartMuteSamples;
      tunerActive.store(rtlConnected, std::memory_order_release);
    }

    if (!tunerActive) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      continue;
    }

    if (runtime_loop::handleControlAndScan(
            scanEngine, xdrServer, requestedFrequencyHz, pendingFrequency,
            pendingGain, pendingAGC, requestedBandwidthHz, pendingBandwidth,
            appliedBandwidthHz, rtlConnected, verboseLogging, audioOut,
            dspRuntime, rdsWorker, dspPipeline, kRetuneMuteSamples,
            retuneMuteSamplesRemaining, retuneMuteTotalSamples,
            [&](const char *reason) { applyRtlGainAndAgc(reason); },
            tunerSetFrequency, tunerReadIQ, writeIqCapture, scanRetrySleep,
            iqBuffer, SDR_BUF_SAMPLES, iqSampleRate, effectiveAppliedGainDb(),
            kSignalGainCompFactor, config.sdr, restoreAfterScan,
            [&]() { return g_running.load(); })) {
      continue;
    }

    int targetDeemphasis = requestedDeemphasis.load();
    if (targetDeemphasis != appliedDeemphasis) {
      dspPipeline.setDeemphasisMode(targetDeemphasis);
      appliedDeemphasis = targetDeemphasis;
    }

    bool targetForceMono = requestedForceMono.load();
    if (targetForceMono != appliedForceMono) {
      appliedForceMono = targetForceMono;
    }

    if (pendingBlendMode.exchange(false)) {
      const int blend = std::clamp(requestedBlendMode.load(), 0, 2);
      StereoDecoder::BlendMode mode = StereoDecoder::BlendMode::Normal;
      const char *label = "normal";
      if (blend == 0) {
        mode = StereoDecoder::BlendMode::Soft;
        label = "soft";
      } else if (blend == 2) {
        mode = StereoDecoder::BlendMode::Aggressive;
        label = "aggressive";
      }
      dspPipeline.setBlendMode(mode);
      if (verboseLogging) {
        std::cout << "[XDR] stereo blend -> " << label << "\n";
      }
    }

    size_t samples = 0;
    if (!readIqSamples(tunerReadIQ, writeIqCapture, iqBuffer, SDR_BUF_SAMPLES,
                       noDataSleep, tunerSession, verboseLogging, samples)) {
      continue;
    }

    (void)processing_runner::processAudioBlock(
        iqBuffer, samples, OUTPUT_RATE, iqSampleRate, appliedBandwidthHz,
        effectiveAppliedGainDb(),
        kSignalGainCompFactor, config, verboseLogging, rfLevelSmoother,
        [&](const SignalLevelResult &signal, double clipRatio,
            float rfLevelFiltered) {
          runtime_loop::maybeAdjustAutoGain(
              useSdrppGainStrategy, gain, isImsAgcEnabled(), requestedAGCMode,
              pendingAGC, lastGainDown, lastGainUp, signal, clipRatio,
              rfLevelFiltered, verboseLogging, agcModeToGainDb);
          runtime_loop::maybeAdjustAdaptiveBandwidth(
              adaptiveBwMode, adaptiveBwState, requestedBandwidthHz,
              pendingBandwidth, appliedBandwidthHz, signal, verboseLogging);
        },
        targetForceMono, appliedEffectiveForceMono, dspPipeline, rdsWorker,
        xdrServer, retuneMuteSamplesRemaining, retuneMuteTotalSamples, audioOut,
        &mpxWavOut, m_options.mpxAudioEnabled ? &mpxAudioOut : nullptr);
  }

  rdsWorker.stop();

  mpxAudioOut.shutdown();
  shutdownResources(audioOut, iqHandle, mpxWavOut, xdrServer, tunerSession);

  std::cout << "[APP] shutdown complete.\n";
  return 0;
}
