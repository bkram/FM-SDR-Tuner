#include "application.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <thread>
#include <vector>

#include "audio_output.h"
#include "cpu_features.h"
#include "dsp/runtime.h"
#include "dsp_pipeline.h"
#include "mpx_audio_output.h"
#include "processing_runner.h"
#include "rds_worker.h"
#include "rest_server.h"
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

  if (tunerSource == "sdrplay") {
    // The RSP front end is configured to deliver INPUT_RATE directly
    // (2.048 MHz / 8), so the pipeline runs at 1:1 IQ decimation regardless of
    // any iq_sample_rate the user set (which only applies to RTL sources).
    iqSampleRate = static_cast<uint32_t>(INPUT_RATE);
  }

  const size_t iqDecimation = static_cast<size_t>(iqSampleRate / INPUT_RATE);

  signal(SIGINT, signalHandler);
  signal(SIGTERM, signalHandler);

  TunerController tuner(tunerSource, tcpHost, tcpPort, rtlDeviceIndex);
  tuner.setLowLatencyMode(lowLatencyIq);
  if (tuner.isSdrPlay()) {
    tuner.configureSdrplay(config.sdrplay.lna_state, config.sdrplay.antenna,
                           config.sdrplay.bias_tee);
  }
  const bool useDirectRtlSdr = tuner.isDirectRtlSdr();
  const bool sourceIsComplex =
      tuner.nativeFormat() == TunerController::IqFormat::CF32;
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
    // SDRplay with hardware AGC enabled: use the RSP's own IF AGC and ignore the
    // RTL-centric manual/TEF gain path. The LNA state is set via configureSdrplay
    // at connect.
    if (tuner.isSdrPlay() && config.sdrplay.agc) {
      tuner.setGainMode(false); // auto-gain mode
      tuner.setAGC(true);       // RSP hardware IF AGC
      if (verboseLogging) {
        std::cout << "[SDR] " << reason << " sdrplay AGC on, LNA "
                  << config.sdrplay.lna_state << "\n";
      }
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

  // Anonymous REST control API (optional; for an fm-dx-webserver plugin). It
  // carries the SDR-specific settings XDR has no vocabulary for (manual dB
  // gain, SDRplay LNA/antenna/bias-tee) plus the common ones, and drives the
  // same TunerController + request-state used by the XDR path so both backends
  // behave identically.
  // Live signal telemetry for /api/status, updated each block on the processing
  // thread and read on the REST thread (hence atomic). overload mirrors the
  // auto-gain overload condition so a client can warn on front-end / ADC clip.
  std::atomic<float> liveSignalLevel{0.0f};
  std::atomic<double> liveSignalDbfs{-120.0};
  std::atomic<double> liveClipRatio{0.0};
  std::atomic<bool> liveOverload{false};
  // Pilot / stereo / MPX telemetry (updated on the processing thread).
  std::atomic<float> livePilotKHz{0.0f};
  std::atomic<float> liveRdsDevKHz{0.0f};
  std::atomic<bool> liveStereo{false};
  std::atomic<float> liveStereoQuality{0.0f};
  std::atomic<float> liveDemodSnrDb{0.0f};
  std::atomic<float> liveMpxMagnitude{0.0f};
  // Peak composite deviation (kHz) — MAX DEV.
  std::atomic<double> liveMpxPeakKhz{0.0};
  // Set by the REST "reset_stats" action; consumed on the processing thread to
  // restart the MPX 60 s window and peak hold.
  std::atomic<bool> statsResetRequest{false};
  // RDS statistics (updated on the RDS worker thread). BER is an EMA of the
  // per-group block-error fraction (same definition as MPXPrime's blockErrorRate
  // = failed/received, but windowed so it tracks current link quality and
  // self-resets on a station change).
  std::atomic<int> liveRdsPi{0};
  std::atomic<double> liveRdsBer{0.0};
  std::atomic<unsigned long long> liveRdsGroups{0};

  // The SDR-specific settables (gain dB / auto-gain / LNA / antenna / bias-T /
  // ppm / RTL-AGC) have no shared atomic the way frequency/volume/etc. do, so
  // we mirror them here for /api/status. All REST handlers run serialized on the
  // RestServer accept thread, so plain members are safe (no atomics). Declared
  // in the outer scope so it outlives restServer (whose lambdas reference it).
  struct RestPanelState {
    double gainDb;
    bool autoGain;
    int lna;
    int antenna;
    bool biasTee;
    int ppm;
    bool rtlAgc;
  };
  RestPanelState restPanel{config.sdr.rtl_gain_db >= 0
                               ? static_cast<double>(config.sdr.rtl_gain_db)
                               : 20.0,
                           (config.sdr.rtl_gain_db < 0) ||
                               (tuner.isSdrPlay() && config.sdrplay.agc),
                           config.sdrplay.lna_state,
                           config.sdrplay.antenna,
                           config.sdrplay.bias_tee,
                           freqCorrectionPpm,
                           config.sdr.sdrpp_rtl_agc};

  // Blend mode reported by /api/status when no live override is set yet
  // (requestedBlendMode == -1): map the INI stereo_blend string to 0/1/2.
  int defaultBlendMode = 1; // normal
  {
    std::string b = config.processing.stereo_blend;
    std::transform(b.begin(), b.end(), b.begin(), [](unsigned char c) {
      return static_cast<char>(std::tolower(c));
    });
    if (b == "soft") {
      defaultBlendMode = 0;
    } else if (b == "aggressive") {
      defaultBlendMode = 2;
    }
  }

  std::unique_ptr<RestServer> restServer;
  if (config.rest.enabled && config.rest.port != 0) {
    RestServer::Controls controls;
    controls.setFrequencyHz = [&](uint32_t hz) {
      requestedFrequencyHz.store(hz, std::memory_order_relaxed);
      pendingFrequency.store(true, std::memory_order_release);
      return true;
    };
    controls.setGainDb = [&](double db) {
      restPanel.gainDb = std::clamp(db, 0.0, 50.0);
      restPanel.autoGain = false;
      tuner.setGainMode(true);
      return tuner.setGain(
          static_cast<uint32_t>(std::lround(restPanel.gainDb * 10.0)));
    };
    controls.setAutoGain = [&](bool on) {
      restPanel.autoGain = on;
      tuner.setGainMode(!on);
      return tuner.setAGC(on);
    };
    controls.setBandwidthHz = [&](int hz) {
      requestedBandwidthHz = hz;
      pendingBandwidth = true;
      return true;
    };
    controls.setLnaState = [&](int s) {
      restPanel.lna = s;
      return tuner.setLnaState(s);
    };
    controls.setAntenna = [&](int i) {
      restPanel.antenna = i;
      return tuner.setAntenna(i);
    };
    controls.setBiasTee = [&](bool e) {
      restPanel.biasTee = e;
      return tuner.setBiasTee(e);
    };
    controls.setPpm = [&](int ppm) {
      restPanel.ppm = ppm;
      return tuner.setFrequencyCorrection(ppm);
    };
    controls.setRtlAgc = [&](bool e) {
      restPanel.rtlAgc = e;
      return tuner.setAGC(e);
    };
    controls.setDeemphasis = [&](int m) {
      if (m < 0 || m > 2) return false;
      requestedDeemphasis.store(m, std::memory_order_relaxed);
      return true;
    };
    controls.setBlendMode = [&](int m) {
      if (m < 0 || m > 2) return false;
      requestedBlendMode.store(m, std::memory_order_relaxed);
      pendingBlendMode.store(true, std::memory_order_release);
      return true;
    };
    controls.setForceMono = [&](bool b) {
      requestedForceMono.store(b, std::memory_order_relaxed);
      return true;
    };
    controls.setVolume = [&](int v) {
      const int clamped = std::clamp(v, 0, 100);
      requestedVolume.store(clamped, std::memory_order_relaxed);
      audioOut.setVolumePercent(clamped);
      return true;
    };
    controls.start = [&]() {
      pendingStartRequest.store(true, std::memory_order_release);
      return true;
    };
    controls.stop = [&]() {
      pendingStopRequest.store(true, std::memory_order_release);
      return true;
    };
    controls.resetStats = [&]() {
      statsResetRequest.store(true, std::memory_order_release);
      liveRdsBer.store(0.0, std::memory_order_relaxed);
      liveRdsGroups.store(0, std::memory_order_relaxed);
      liveRdsPi.store(0, std::memory_order_relaxed);
      return true;
    };
    controls.statusJson = [&]() -> std::string {
      std::ostringstream oss;
      // All metrics reported to one decimal (N.N); the small ratios clip/rds_ber
      // keep more precision so they don't collapse to 0.0.
      oss << std::fixed << std::setprecision(1);
      oss << "{\"source\":\"" << tuner.name() << "\",\"model\":\""
          << tuner.modelName() << "\",\"antenna_count\":" << tuner.antennaCount()
          << ",\"running\":" << (tunerActive.load() ? "true" : "false")
          << ",\"frequency_hz\":" << requestedFrequencyHz.load()
          << ",\"bandwidth_hz\":" << appliedBandwidthHz
          << ",\"gain_db\":" << restPanel.gainDb
          << ",\"auto_gain\":" << (restPanel.autoGain ? "true" : "false")
          << ",\"lna\":" << restPanel.lna << ",\"antenna\":" << restPanel.antenna
          << ",\"bias_tee\":" << (restPanel.biasTee ? "true" : "false")
          << ",\"ppm\":" << restPanel.ppm
          << ",\"rtl_agc\":" << (restPanel.rtlAgc ? "true" : "false")
          << ",\"deemphasis\":" << requestedDeemphasis.load()
          << ",\"blend\":"
          << (requestedBlendMode.load() >= 0 ? requestedBlendMode.load()
                                             : defaultBlendMode)
          << ",\"force_mono\":" << (requestedForceMono.load() ? "true" : "false")
          << ",\"volume\":" << requestedVolume.load()
          << ",\"signal\":" << liveSignalLevel.load(std::memory_order_relaxed)
          << ",\"dbfs\":" << liveSignalDbfs.load(std::memory_order_relaxed)
          << ",\"clip\":" << std::setprecision(4)
          << liveClipRatio.load(std::memory_order_relaxed) << std::setprecision(1)
          << ",\"overload\":"
          << (liveOverload.load(std::memory_order_relaxed) ? "true" : "false")
          << ",\"stereo\":"
          << (liveStereo.load(std::memory_order_relaxed) ? "true" : "false")
          << ",\"pilot_khz\":"
          << livePilotKHz.load(std::memory_order_relaxed)
          << ",\"rds_dev_khz\":"
          << liveRdsDevKHz.load(std::memory_order_relaxed)
          << ",\"stereo_quality\":"
          << liveStereoQuality.load(std::memory_order_relaxed)
          << ",\"snr\":" << liveDemodSnrDb.load(std::memory_order_relaxed)
          << ",\"mpx\":" << liveMpxMagnitude.load(std::memory_order_relaxed)
          << ",\"mpx_peak_khz\":"
          << liveMpxPeakKhz.load(std::memory_order_relaxed)
          << ",\"rds_ber\":" << std::setprecision(4)
          << liveRdsBer.load(std::memory_order_relaxed) << std::setprecision(1)
          << ",\"rds_groups\":" << liveRdsGroups.load(std::memory_order_relaxed)
          << "}";
      return oss.str();
    };
    restServer = std::make_unique<RestServer>(config.rest.bind_address,
                                              config.rest.port, controls);
    restServer->setVerboseLogging(verboseLogging);
    if (!restServer->start()) {
      std::cerr << "[REST] failed to start REST control API\n";
      restServer.reset();
    }
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
    // RDS telemetry for /api/status. errors packs 2 bits per block
    // (0=ok, 1=errored, 3=missing); a block is "valid" only when its field is 0.
    const uint8_t e = group.errors;
    auto fld = [&](int shift) { return (e >> shift) & 0x3; };
    const int erroredBlocks = (fld(6) != 0) + (fld(4) != 0) + (fld(2) != 0) +
                              (fld(0) != 0);
    const double frac = static_cast<double>(erroredBlocks) / 4.0;
    const double prev = liveRdsBer.load(std::memory_order_relaxed);
    liveRdsBer.store(prev * 0.95 + frac * 0.05, std::memory_order_relaxed);
    liveRdsGroups.fetch_add(1, std::memory_order_relaxed);
    if (group.blockA != 0) {
      liveRdsPi.store(group.blockA, std::memory_order_relaxed); // PI = block A
    }
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
  // CF32 sources (SDRplay) deliver full-precision complex<float> for the demod;
  // we also quantize them into iqBuffer as an 8-bit shadow so the signal meter,
  // scan engine, and IQ capture (all RTL-format consumers) work unchanged.
  std::vector<std::complex<float>> iqComplexStorage;
  if (sourceIsComplex) {
    iqComplexStorage.assign(SDR_BUF_SAMPLES, std::complex<float>(0.0f, 0.0f));
  }

  // Pre-armed so the very first processed samples after startup (including
  // the --auto-start path, which connects before this loop) get the same
  // settle mute as a retune. The counter only decrements while samples flow.
  size_t retuneMuteSamplesRemaining = kStartMuteSamples;
  size_t retuneMuteTotalSamples = kStartMuteSamples;

  // SDRplay wide-bandwidth scan mode edge state (see runtime_loop scan logic).
  bool scanWideActive = false;
  uint32_t scanWideRate = iqSampleRate;

  // MAX DEV state: ~1 s decaying peak hold of the composite deviation.
  // Block time ≈ dspBlockSize / INPUT_RATE.
  const double kMpxBlockSec =
      static_cast<double>(dspBlockSize) / static_cast<double>(INPUT_RATE);
  const float kMpxPeakDecay = static_cast<float>(std::exp(-kMpxBlockSec / 1.0));
  constexpr double kMpxDevFullScaleKHz = 75.0; // demod 1.0 == 75 kHz
  float mpxPeakHold = 0.0f;

  ScanEngine scanEngine;
  auto lastGainDown =
      std::chrono::steady_clock::now() - std::chrono::seconds(5);
  auto lastGainUp = std::chrono::steady_clock::now() - std::chrono::seconds(5);
  // Periodic device-state keep-alive (see the retune/reassert note in
  // runtime_loop.cpp). When parked on one frequency for hours — the typical
  // FM-DX-webserver deployment — no retune ever re-asserts the tuner's
  // gain/AGC/mode registers, so silently lost or drifted device state used to
  // persist until an app restart. Rewriting the same register values is
  // inaudible (the auto-gain path already does live gain writes routinely).
  constexpr auto kDeviceReassertInterval = std::chrono::minutes(5);
  auto lastDeviceReassert = std::chrono::steady_clock::now();
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
      // Idempotent start: if the tuner is already playing (e.g. --auto-start,
      // or another client already started it), ignore the redundant start so a
      // newly-connecting client doesn't restart the DSP/audio (which reads as a
      // glitch / "second player"). Only (re)connect when not already active —
      // this still lets a client retry after a failed auto-start, or restart
      // after an explicit stop.
      if (!tunerActive.load(std::memory_order_acquire)) {
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
    }

    if (!tunerActive) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      continue;
    }

    if (rtlConnected && !scanWideActive && !scanEngine.isActive() &&
        (std::chrono::steady_clock::now() - lastDeviceReassert) >=
            kDeviceReassertInterval) {
      lastDeviceReassert = std::chrono::steady_clock::now();
      applyRtlGainAndAgc("periodic/reassert");
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
            [&]() { return g_running.load(); },
            [&](bool wide) { return tuner.setScanWideMode(wide); },
            scanWideActive, scanWideRate, [&]() { tuner.flushBuffers(); })) {
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
    const std::complex<float> *iqComplexPtr = nullptr;
    if (sourceIsComplex) {
      samples = tuner.readIQ(iqComplexStorage.data(), SDR_BUF_SAMPLES);
      if (samples == 0) {
        tunerSession.noteReadFailureAndMaybeReconnect();
        std::this_thread::sleep_for(noDataSleep);
        continue;
      }
      for (size_t i = 0; i < samples; i++) {
        const float si = std::lround(iqComplexStorage[i].real() * 127.5f + 127.5f);
        const float sq = std::lround(iqComplexStorage[i].imag() * 127.5f + 127.5f);
        iqBuffer[2 * i] = static_cast<uint8_t>(std::clamp(si, 0.0f, 255.0f));
        iqBuffer[2 * i + 1] = static_cast<uint8_t>(std::clamp(sq, 0.0f, 255.0f));
      }
      writeIqCapture(iqBuffer, samples);
      tunerSession.resetReadFailures();
      iqComplexPtr = iqComplexStorage.data();
    } else if (!readIqSamples(tunerReadIQ, writeIqCapture, iqBuffer,
                              SDR_BUF_SAMPLES, noDataSleep, tunerSession,
                              verboseLogging, samples)) {
      continue;
    }

    (void)processing_runner::processAudioBlock(
        iqBuffer, samples, OUTPUT_RATE, iqSampleRate, appliedBandwidthHz,
        effectiveAppliedGainDb(),
        kSignalGainCompFactor, config, verboseLogging, rfLevelSmoother,
        [&](const SignalLevelResult &signal, double clipRatio,
            float rfLevelFiltered) {
          // Publish live telemetry for the REST API. Overload uses the same
          // condition the auto-gain loop acts on (heavy IQ clipping or channel
          // power into the top few dB of full scale).
          liveSignalLevel.store(rfLevelFiltered, std::memory_order_relaxed);
          liveSignalDbfs.store(signal.dbfs, std::memory_order_relaxed);
          liveClipRatio.store(clipRatio, std::memory_order_relaxed);
          liveOverload.store((clipRatio > 0.0200) || (signal.dbfs > -5.0),
                             std::memory_order_relaxed);
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
        &mpxWavOut, m_options.mpxAudioEnabled ? &mpxAudioOut : nullptr,
        iqComplexPtr,
        [&](float pilotKHz, bool stereo, float quality, float mpxMag,
            float mpxPeak, float rdsDevKHz, float demodSnrDb) {
          livePilotKHz.store(pilotKHz, std::memory_order_relaxed);
          liveRdsDevKHz.store(rdsDevKHz, std::memory_order_relaxed);
          liveStereo.store(stereo, std::memory_order_relaxed);
          liveStereoQuality.store(quality, std::memory_order_relaxed);
          liveDemodSnrDb.store(demodSnrDb, std::memory_order_relaxed);
          liveMpxMagnitude.store(mpxMag, std::memory_order_relaxed);
          // MAX DEV: ~1 s decaying peak hold of the composite deviation.
          if (statsResetRequest.exchange(false, std::memory_order_acquire)) {
            mpxPeakHold = 0.0f;
          }
          mpxPeakHold = std::max(mpxPeak, mpxPeakHold * kMpxPeakDecay);
          liveMpxPeakKhz.store(
              static_cast<double>(mpxPeakHold) * kMpxDevFullScaleKHz,
              std::memory_order_relaxed);
        });
  }

  rdsWorker.stop();

  if (restServer) {
    restServer->stop();
  }
  mpxAudioOut.shutdown();
  shutdownResources(audioOut, iqHandle, mpxWavOut, xdrServer, tunerSession);

  std::cout << "[APP] shutdown complete.\n";
  return 0;
}
