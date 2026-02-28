#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <deque>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) ||             \
    defined(_M_IX86)
#include <immintrin.h>
#endif
#if defined(__aarch64__) || defined(__arm__) || defined(_M_ARM64)
#include <arm_neon.h>
#endif

#include "af_post_processor.h"
#include "audio_output.h"
#include "config.h"
#include "cpu_features.h"
#include "dsp/liquid_primitives.h"
#include "dsp/runtime.h"
#include "fm_demod.h"
#include "rds_decoder.h"
#include "rtl_sdr_device.h"
#include "rtl_tcp_client.h"
#include "signal_level.h"
#include "stereo_decoder.h"
#include "xdr_server.h"

static std::atomic<bool> g_running(true);

void signalHandler(int) { g_running = false; }

void printUsage(const char *prog) {
  std::cout
      << "Usage: " << prog << " [options]\n"
      << "Options:\n"
      << "  -c, --config <file>    INI config file\n"
      << "  -t, --tcp <host:port>  rtl_tcp server address (default: "
         "localhost:1234)\n"
      << "      --iq-rate <rate>   IQ sample rate: 256000, 1024000, or 2048000 "
         "(default: 256000)\n"
      << "      --source <name>    Tuner source: rtl_tcp or rtl_sdr (default: "
         "rtl_sdr)\n"
      << "      --rtl-device <id>  RTL-SDR device index for --source rtl_sdr "
         "(default: 0)\n"
      << "  -f, --freq <khz>      Frequency in kHz (default: 88600)\n"
      << "  -g, --gain <db>       RTL-SDR gain in dB (default: auto)\n"
      << "  -w, --wav <file>      Output WAV file\n"
      << "  -i, --iq <file>       Capture raw IQ bytes to file\n"
      << "      --low-latency-iq  Keep newest IQ samples (drop backlog on "
         "overload)\n"
      << "  -s, --audio           Enable audio output\n"
      << "  -l, --list-audio      List available audio output devices\n"
      << "  -d, --device <id>     Audio output device (index or name)\n"
      << "  -P, --password <pwd>   XDR server password\n"
      << "  -G, --guest            Enable guest mode (no password required)\n"
      << "  -h, --help             Show this help\n";
}

int main(int argc, char *argv[]) {
  constexpr int INPUT_RATE = 256000;
  constexpr int OUTPUT_RATE = 32000;

  std::cout << "FM-SDR-Tuner version " << FM_SDR_TUNER_VERSION << "\n"
            << "Copyright 2026 by Bkram Developments\n";

  const CPUFeatures cpu = detectCPUFeatures();

  std::string configPath;
  for (int i = 1; i < argc; i++) {
    const std::string arg = argv[i];
    if (arg == "-c" && i + 1 < argc) {
      configPath = argv[++i];
      continue;
    }
    static constexpr const char *kConfigPrefix = "--config=";
    if (arg.rfind(kConfigPrefix, 0) == 0) {
      configPath = arg.substr(std::strlen(kConfigPrefix));
      continue;
    }
    if (arg == "--config" && i + 1 < argc) {
      configPath = argv[++i];
      continue;
    }
  }

  Config config;
  config.loadDefaults();
  if (!configPath.empty() && !config.loadFromFile(configPath)) {
    return 1;
  }
  const bool verboseLogging = config.debug.log_level > 0;
  if (verboseLogging) {
    std::cout << "[CPU] " << cpu.summary() << "\n";
  }
  if (verboseLogging && !configPath.empty()) {
    std::cout << "[Config] loaded: " << configPath << "\n";
  }
  if (verboseLogging) {
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
    std::cout << "[Config] sdr.freq_correction_ppm="
              << config.sdr.freq_correction_ppm << "\n";
    std::cout << "[Config] sdr.low_latency_iq="
              << (config.sdr.low_latency_iq ? "true" : "false") << "\n";
    std::cout << "[Config] rtl_tcp.sample_rate=" << config.rtl_tcp.sample_rate
              << "\n";
  }
  std::string tcpHost = config.rtl_tcp.host;
  uint16_t tcpPort = config.rtl_tcp.port;
  uint32_t iqSampleRate = config.rtl_tcp.sample_rate;
  std::string tunerSource = config.tuner.source;
  uint32_t rtlDeviceIndex = config.tuner.rtl_device;
  uint32_t freqKHz = config.tuner.default_freq;
  int gain = config.sdr.rtl_gain_db;
  const int freqCorrectionPpm =
      std::clamp(config.sdr.freq_correction_ppm, -250, 250);
  const bool useSdrppGainStrategy = (config.sdr.gain_strategy == "sdrpp");
  const int defaultCustomGainFlags = config.sdr.default_custom_gain_flags;
  bool allowClientGainOverride = config.processing.client_gain_allowed;
  std::string wavFile;
  std::string iqFile;
  bool enableSpeaker = config.audio.enable_audio;
  std::string audioDevice;
  std::string xdrPassword = config.xdr.password;
  bool xdrGuestMode = config.xdr.guest_mode;
  uint16_t xdrPort = config.xdr.port;
  bool autoReconnect = config.reconnection.auto_reconnect;
  bool lowLatencyIq = config.sdr.low_latency_iq;

  auto parseTcpOption = [&](const std::string &value) -> bool {
    size_t colon = value.find(':');
    if (colon != std::string::npos) {
      tcpHost = value.substr(0, colon);
      try {
        const int parsedPort = std::stoi(value.substr(colon + 1));
        if (parsedPort < 1 || parsedPort > 65535) {
          std::cerr << "[CLI] invalid tcp port: " << parsedPort << "\n";
          return false;
        }
        tcpPort = static_cast<uint16_t>(parsedPort);
      } catch (...) {
        std::cerr << "[CLI] invalid --tcp value: " << value << "\n";
        return false;
      }
    } else {
      tcpHost = value;
    }
    return true;
  };

  auto parseIntOption = [&](const std::string &name, const std::string &value,
                            int &out) -> bool {
    try {
      out = std::stoi(value);
      return true;
    } catch (...) {
      std::cerr << "[CLI] invalid --" << name << " value: " << value << "\n";
      return false;
    }
  };

  auto parseUIntFreqKHz = [&](const std::string &value) -> bool {
    try {
      const int parsedFreq = std::stoi(value);
      if (parsedFreq <= 0) {
        std::cerr << "[CLI] invalid frequency kHz: " << parsedFreq << "\n";
        return false;
      }
      freqKHz = static_cast<uint32_t>(parsedFreq);
      return true;
    } catch (...) {
      std::cerr << "[CLI] invalid --freq value: " << value << "\n";
      return false;
    }
  };

  auto parseSourceOption = [&](std::string value) -> bool {
    std::transform(
        value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (value == "rtl_tcp" || value == "tcp") {
      tunerSource = "rtl_tcp";
      return true;
    }
    if (value == "rtl_sdr" || value == "sdr") {
      tunerSource = "rtl_sdr";
      return true;
    }
    std::cerr << "[CLI] invalid source value: " << value
              << " (expected rtl_tcp or rtl_sdr)\n";
    return false;
  };

  if (!parseSourceOption(tunerSource)) {
    std::cerr << "[Config] invalid tuner.source: " << config.tuner.source
              << " (expected rtl_tcp or rtl_sdr), using rtl_sdr\n";
    tunerSource = "rtl_sdr";
  }

  auto parseDeviceIndexOption = [&](const std::string &value) -> bool {
    try {
      const int parsed = std::stoi(value);
      if (parsed < 0) {
        std::cerr << "[CLI] invalid --rtl-device value: " << value << "\n";
        return false;
      }
      rtlDeviceIndex = static_cast<uint32_t>(parsed);
      return true;
    } catch (...) {
      std::cerr << "[CLI] invalid --rtl-device value: " << value << "\n";
      return false;
    }
  };

  auto readValue = [&](int &index, const std::string &current,
                       const std::string &longName) -> std::string {
    const std::string prefix = "--" + longName + "=";
    if (current.rfind(prefix, 0) == 0) {
      return current.substr(prefix.length());
    }
    if (index + 1 < argc) {
      index++;
      return argv[index];
    }
    return std::string();
  };

  for (int i = 1; i < argc; i++) {
    const std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      printUsage(argv[0]);
      return 0;
    }
    if (arg == "-s" || arg == "--audio") {
      enableSpeaker = true;
      continue;
    }
    if (arg == "--low-latency-iq") {
      lowLatencyIq = true;
      continue;
    }
    if (arg == "--no-low-latency-iq") {
      lowLatencyIq = false;
      continue;
    }
    if (arg == "-G" || arg == "--guest") {
      xdrGuestMode = true;
      continue;
    }
    if (arg == "-l" || arg == "--list-audio") {
      if (!AudioOutput::listDevices()) {
        return 1;
      }
      return 0;
    }

    if (arg == "-c" || arg == "--config" || arg.rfind("--config=", 0) == 0) {
      const std::string value = readValue(i, arg, "config");
      if (value.empty()) {
        std::cerr << "[CLI] missing value for --config\n";
        return 1;
      }
      configPath = value;
      continue;
    }
    if (arg == "-t" || arg == "--tcp" || arg.rfind("--tcp=", 0) == 0) {
      const std::string value = readValue(i, arg, "tcp");
      if (value.empty() || !parseTcpOption(value)) {
        return 1;
      }
      continue;
    }
    if (arg == "--iq-rate" || arg.rfind("--iq-rate=", 0) == 0) {
      const std::string value = readValue(i, arg, "iq-rate");
      try {
        const int parsed = std::stoi(value);
        if (parsed == 256000 || parsed == 1024000 || parsed == 2048000) {
          iqSampleRate = static_cast<uint32_t>(parsed);
        } else {
          std::cerr << "[CLI] invalid --iq-rate value: " << value
                    << " (expected 256000, 1024000, or 2048000)\n";
          return 1;
        }
      } catch (...) {
        std::cerr << "[CLI] invalid --iq-rate value: " << value << "\n";
        return 1;
      }
      continue;
    }
    if (arg == "--source" || arg.rfind("--source=", 0) == 0) {
      const std::string value = readValue(i, arg, "source");
      if (value.empty() || !parseSourceOption(value)) {
        return 1;
      }
      continue;
    }
    if (arg == "--rtl-device" || arg.rfind("--rtl-device=", 0) == 0) {
      const std::string value = readValue(i, arg, "rtl-device");
      if (value.empty() || !parseDeviceIndexOption(value)) {
        return 1;
      }
      continue;
    }
    if (arg == "-f" || arg == "--freq" || arg.rfind("--freq=", 0) == 0) {
      const std::string value = readValue(i, arg, "freq");
      if (value.empty() || !parseUIntFreqKHz(value)) {
        return 1;
      }
      continue;
    }
    if (arg == "-g" || arg == "--gain" || arg.rfind("--gain=", 0) == 0) {
      const std::string value = readValue(i, arg, "gain");
      if (value.empty() || !parseIntOption("gain", value, gain)) {
        return 1;
      }
      continue;
    }
    if (arg == "-w" || arg == "--wav" || arg.rfind("--wav=", 0) == 0) {
      const std::string value = readValue(i, arg, "wav");
      if (value.empty()) {
        std::cerr << "[CLI] missing value for --wav\n";
        return 1;
      }
      wavFile = value;
      continue;
    }
    if (arg == "-i" || arg == "--iq" || arg.rfind("--iq=", 0) == 0) {
      const std::string value = readValue(i, arg, "iq");
      if (value.empty()) {
        std::cerr << "[CLI] missing value for --iq\n";
        return 1;
      }
      iqFile = value;
      continue;
    }
    if (arg == "-d" || arg == "--device" || arg.rfind("--device=", 0) == 0) {
      const std::string value = readValue(i, arg, "device");
      if (value.empty()) {
        std::cerr << "[CLI] missing value for --device\n";
        return 1;
      }
      audioDevice = value;
      continue;
    }
    if (arg == "-P" || arg == "--password" ||
        arg.rfind("--password=", 0) == 0) {
      const std::string value = readValue(i, arg, "password");
      if (value.empty()) {
        std::cerr << "[CLI] missing value for --password\n";
        return 1;
      }
      xdrPassword = value;
      continue;
    }

    std::cerr << "[CLI] unknown option: " << arg << "\n";
    printUsage(argv[0]);
    return 1;
  }

  if (wavFile.empty() && iqFile.empty() && !enableSpeaker) {
    std::cerr << "[CLI] error: must specify at least one output: -w (wav), -i "
                 "(iq), or -s (audio)\n";
    printUsage(argv[0]);
    return 1;
  }

  if (!(iqSampleRate == 256000 || iqSampleRate == 1024000 ||
        iqSampleRate == 2048000)) {
    std::cerr << "[SDR] unsupported iq sample rate: " << iqSampleRate
              << " (expected 256000, 1024000, or 2048000)\n";
    return 1;
  }
  const size_t iqDecimation = static_cast<size_t>(iqSampleRate / INPUT_RATE);
  if (iqDecimation == 0 || (iqSampleRate % INPUT_RATE) != 0) {
    std::cerr << "[SDR] iq sample rate must be an integer multiple of "
              << INPUT_RATE << "\n";
    return 1;
  }

  signal(SIGINT, signalHandler);
  signal(SIGTERM, signalHandler);

  RTLTCPClient rtlTcpClient(tcpHost, tcpPort);
  RTLSDRDevice rtlSdrDevice(rtlDeviceIndex);
  rtlSdrDevice.setLowLatencyMode(lowLatencyIq);
  const bool useDirectRtlSdr = (tunerSource == "rtl_sdr");
  bool rtlConnected = false;
  std::atomic<uint32_t> requestedFrequencyHz(freqKHz * 1000);
  std::atomic<int> requestedCustomGain(defaultCustomGainFlags);
  std::atomic<int> requestedAGCMode(
      std::clamp(config.processing.agc_mode, 0, 3));
  std::atomic<int> requestedBandwidthHz(0);
  std::atomic<int> requestedVolume(
      std::clamp(config.audio.startup_volume, 0, 100));
  std::atomic<int> requestedDeemphasis(
      std::clamp(config.tuner.deemphasis, 0, 2));
  std::atomic<bool> requestedForceMono(false);
  std::atomic<bool> pendingFrequency(false);
  std::atomic<bool> pendingGain(false);
  std::atomic<bool> pendingAGC(false);
  std::atomic<bool> pendingBandwidth(false);

  auto tunerName = [&]() -> const char * {
    return useDirectRtlSdr ? "rtl_sdr" : "rtl_tcp";
  };
  if (verboseLogging && useDirectRtlSdr) {
    std::cout << "[SDR] low-latency IQ mode: "
              << (lowLatencyIq ? "enabled" : "disabled") << "\n";
  }
  auto tunerConnect = [&]() -> bool {
    return useDirectRtlSdr ? rtlSdrDevice.connect() : rtlTcpClient.connect();
  };
  auto tunerDisconnect = [&]() {
    if (useDirectRtlSdr) {
      rtlSdrDevice.disconnect();
    } else {
      rtlTcpClient.disconnect();
    }
  };
  auto tunerSetFrequency = [&](uint32_t freqHz) -> bool {
    return useDirectRtlSdr ? rtlSdrDevice.setFrequency(freqHz)
                           : rtlTcpClient.setFrequency(freqHz);
  };
  auto tunerSetSampleRate = [&](uint32_t sampleRate) -> bool {
    return useDirectRtlSdr ? rtlSdrDevice.setSampleRate(sampleRate)
                           : rtlTcpClient.setSampleRate(sampleRate);
  };
  auto tunerSetFrequencyCorrection = [&](int ppm) -> bool {
    return useDirectRtlSdr ? rtlSdrDevice.setFrequencyCorrection(ppm)
                           : rtlTcpClient.setFrequencyCorrection(ppm);
  };
  auto tunerSetGainMode = [&](bool manual) -> bool {
    return useDirectRtlSdr ? rtlSdrDevice.setGainMode(manual)
                           : rtlTcpClient.setGainMode(manual);
  };
  auto tunerSetGain = [&](uint32_t gainTenthsDb) -> bool {
    return useDirectRtlSdr ? rtlSdrDevice.setGain(gainTenthsDb)
                           : rtlTcpClient.setGain(gainTenthsDb);
  };
  auto tunerSetAgc = [&](bool enable) -> bool {
    return useDirectRtlSdr ? rtlSdrDevice.setAGC(enable)
                           : rtlTcpClient.setAGC(enable);
  };
  auto tunerReadIQ = [&](uint8_t *buffer, size_t maxSamples) -> size_t {
    return useDirectRtlSdr ? rtlSdrDevice.readIQ(buffer, maxSamples)
                           : rtlTcpClient.readIQ(buffer, maxSamples);
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
      // CLI manual override keeps tuner in manual gain mode.
      return false;
    }
    const int custom = requestedCustomGain.load();
    const bool ims = (custom % 10) != 0;
    return ims;
  };

  auto calculateAppliedGainDb = [&]() -> int {
    // TEF-like AGC threshold profile approximation on RTL-SDR.
    static constexpr int kAgcToGainDb[4] = {44, 36, 30, 24}; // highest..low
    const int agcMode = std::clamp(requestedAGCMode.load(), 0, 3);

    int custom = requestedCustomGain.load();
    const bool ceq = ((custom / 10) % 10) != 0;
    int gainDb = kAgcToGainDb[agcMode] + (ceq ? 4 : 0);
    if (gain >= 0) {
      // CLI manual override for debugging.
      gainDb = gain;
    }
    return std::clamp(gainDb, 0, 49);
  };

  auto effectiveAppliedGainDb = [&]() -> int {
    if (isImsAgcEnabled()) {
      // Tuner AGC owns front-end gain; fixed dB compensation is not valid here.
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
      std::cout << "[SDR] " << reason << " A" << agcMode << " G"
                << formatCustomGain(custom) << " (rf=" << rf << ",if=" << ifv
                << ")"
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

  auto connectTuner = [&]() {
    if (!rtlConnected) {
      if (useDirectRtlSdr) {
        std::cout << "[SDR] connecting to rtl_sdr device " << rtlDeviceIndex
                  << "...\n";
      } else {
        std::cout << "[SDR] connecting to rtl_tcp at " << tcpHost << ":"
                  << tcpPort << "...\n";
      }
      if (tunerConnect()) {
        std::cout << "[SDR] connected; setting frequency to " << freqKHz
                  << " kHz...\n";
        const bool okFreq = tunerSetFrequency(requestedFrequencyHz.load());
        const bool okRate = tunerSetSampleRate(iqSampleRate);
        bool okPpm = true;
        if (freqCorrectionPpm != 0) {
          okPpm = tunerSetFrequencyCorrection(freqCorrectionPpm);
        }
        if (!okFreq || !okRate) {
          std::cerr << "[SDR] warning: failed to initialize " << tunerName()
                    << " stream (setFrequency=" << (okFreq ? 1 : 0)
                    << ", setSampleRate=" << (okRate ? 1 : 0)
                    << ", setPpm=" << (okPpm ? 1 : 0) << ")\n";
          tunerDisconnect();
          return;
        }
        if (!okPpm) {
          std::cerr
              << "[SDR] warning: failed to apply frequency correction ppm="
              << freqCorrectionPpm << " (continuing without ppm correction)\n";
        }
        if (verboseLogging) {
          std::cout << "[SDR] applying TEF AGC mode " << requestedAGCMode.load()
                    << " and custom gain flags G" << requestedCustomGain.load()
                    << "...\n";
        }
        rtlConnected = true;
        applyRtlGainAndAgc("connect/apply");
      } else {
        std::cerr << "[SDR] warning: failed to connect to " << tunerName()
                  << "\n";
      }
    }
  };

  auto disconnectTuner = [&]() {
    if (rtlConnected) {
      tunerDisconnect();
      rtlConnected = false;
      std::cout << "[SDR] disconnected from " << tunerName() << "\n";
    }
  };

  FMDemod demod(INPUT_RATE, OUTPUT_RATE);
  demod.setW0BandwidthHz(config.processing.w0_bandwidth_hz);
  {
    std::string dspAgc = config.processing.dsp_agc;
    std::transform(
        dspAgc.begin(), dspAgc.end(), dspAgc.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (dspAgc == "fast") {
      demod.setDspAgcMode(FMDemod::DspAgcMode::Fast);
    } else if (dspAgc == "slow") {
      demod.setDspAgcMode(FMDemod::DspAgcMode::Slow);
    } else {
      demod.setDspAgcMode(FMDemod::DspAgcMode::Off);
    }
  }
  StereoDecoder stereo(INPUT_RATE, OUTPUT_RATE);
  {
    std::string blendMode = config.processing.stereo_blend;
    std::transform(
        blendMode.begin(), blendMode.end(), blendMode.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (blendMode == "soft") {
      stereo.setBlendMode(StereoDecoder::BlendMode::Soft);
    } else if (blendMode == "aggressive") {
      stereo.setBlendMode(StereoDecoder::BlendMode::Aggressive);
    } else {
      stereo.setBlendMode(StereoDecoder::BlendMode::Normal);
    }
  }
  AFPostProcessor afPost(INPUT_RATE, OUTPUT_RATE);
  fm_tuner::dsp::liquid::ComplexDecimator iqDecimator;
  const uint32_t decimFactor = static_cast<uint32_t>(iqDecimation);
  const uint32_t decimTapsPerPhase =
      (decimFactor >= 8U) ? 28U : ((decimFactor >= 4U) ? 20U : 12U);
  iqDecimator.init(decimFactor, decimTapsPerPhase, 80.0f);
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
  dspRuntime.addResetHandler([&]() {
    demod.reset();
    stereo.reset();
    afPost.reset();
    iqDecimator.reset();
  });
  int appliedBandwidthHz = requestedBandwidthHz.load();
  int appliedDeemphasis = requestedDeemphasis.load();
  bool appliedForceMono = requestedForceMono.load();
  bool appliedEffectiveForceMono = appliedForceMono;
  constexpr size_t kRetuneMuteSamples =
      static_cast<size_t>(OUTPUT_RATE / 25); // ~40 ms at 32 kHz
  SignalLevelSmoother rfLevelSmoother;
  if (appliedDeemphasis == 0) {
    afPost.setDeemphasis(50);
    demod.setDeemphasis(50);
  } else if (appliedDeemphasis == 1) {
    afPost.setDeemphasis(75);
    demod.setDeemphasis(75);
  } else {
    afPost.setDeemphasis(0);
    demod.setDeemphasis(0);
  }
  stereo.setForceMono(appliedEffectiveForceMono);
  demod.setBandwidthHz(appliedBandwidthHz);

  AudioOutput audioOut;
  if (verboseLogging) {
    std::cout << "[AUDIO] initializing audio output..." << std::endl;
  }
  const std::string &audioDeviceToUse =
      !audioDevice.empty() ? audioDevice : config.audio.device;
  if (!audioOut.init(enableSpeaker, wavFile, audioDeviceToUse,
                     verboseLogging)) {
    std::cerr << "[AUDIO] failed to initialize audio output\n";
    tunerDisconnect();
    return 1;
  }
  audioOut.setVolumePercent(requestedVolume.load());
  if (verboseLogging) {
    std::cout << "[AUDIO] audio output initialized" << std::endl;
  }

  FILE *iqHandle = nullptr;
  if (!iqFile.empty()) {
    iqHandle = std::fopen(iqFile.c_str(), "wb");
    if (!iqHandle) {
      std::cerr << "[IQ] failed to open IQ output file: " << iqFile << "\n";
      audioOut.shutdown();
      tunerDisconnect();
      return 1;
    }
    if (verboseLogging) {
      std::cout << "[IQ] capture enabled: " << iqFile << "\n";
    }
  }
  auto writeIqCapture = [&](const uint8_t *data, size_t sampleCount) {
    if (!iqHandle || !data || sampleCount == 0) {
      return;
    }
    (void)std::fwrite(data, 1, sampleCount * 2, iqHandle);
  };

  std::atomic<bool> tunerActive(false);
  const bool autoStartForIqCapture = !iqFile.empty();
  const bool autoStartTuner = false;

  XDRServer xdrServer(xdrPort);
  xdrServer.setVerboseLogging(verboseLogging);
  xdrServer.setFrequencyState(requestedFrequencyHz.load());
  if (!xdrPassword.empty()) {
    xdrServer.setPassword(xdrPassword);
  }
  if (xdrGuestMode) {
    xdrServer.setGuestMode(true);
  }
  xdrServer.setFrequencyCallback([&](uint32_t freqHz) {
    if (verboseLogging) {
      std::cout << "[XDR] tuning to " << (freqHz / 1000) << " kHz\n";
    }
    requestedFrequencyHz.store(freqHz, std::memory_order_relaxed);
    pendingFrequency.store(true, std::memory_order_release);
  });
  xdrServer.setVolumeCallback([&](int volume) {
    requestedVolume = std::clamp(volume, 0, 100);
    audioOut.setVolumePercent(requestedVolume.load());
  });
  xdrServer.setGainCallback([&](int newGain) -> bool {
    if (useSdrppGainStrategy) {
      if (verboseLogging) {
        std::cout << "[XDR] G command ignored (gain_strategy=sdrpp)\n";
      }
      return false;
    }
    if (!allowClientGainOverride) {
      if (verboseLogging) {
        std::cout << "[XDR] G command ignored (client_gain_allowed=false)\n";
      }
      return false;
    }
    const int rf = ((newGain / 10) % 10) ? 1 : 0;
    const int ifv = (newGain % 10) ? 1 : 0;
    requestedCustomGain = rf * 10 + ifv;
    if (verboseLogging) {
      std::cout << "[XDR] G" << formatCustomGain(newGain)
                << " received -> rf=" << rf << " if=" << ifv << "\n";
    }
    pendingGain = true;
    return true;
  });
  xdrServer.setAGCCallback([&](int agcMode) -> bool {
    if (useSdrppGainStrategy) {
      if (verboseLogging) {
        std::cout << "[XDR] A command ignored (gain_strategy=sdrpp)\n";
      }
      return false;
    }
    if (!allowClientGainOverride) {
      if (verboseLogging) {
        std::cout << "[XDR] A command ignored (client_gain_allowed=false)\n";
      }
      return false;
    }
    const int clipped = std::clamp(agcMode, 0, 3);
    requestedAGCMode = clipped;
    if (verboseLogging) {
      std::cout << "[XDR] A" << clipped << " received\n";
    }
    pendingAGC = true;
    return true;
  });
  xdrServer.setModeCallback([&](int mode) {
    if (verboseLogging && mode != 0) {
      std::cout << "[XDR] mode " << mode << " requested (FM demod path only)\n";
    }
  });
  xdrServer.setBandwidthCallback([&](int bandwidth) {
    requestedBandwidthHz = std::clamp(bandwidth, 0, 400000);
    pendingBandwidth = true;
    if (verboseLogging) {
      std::cout << "[XDR] W" << requestedBandwidthHz.load() << " received\n";
    }
  });
  xdrServer.setDeemphasisCallback([&](int deemphasis) {
    requestedDeemphasis = std::clamp(deemphasis, 0, 2);
  });
  xdrServer.setForceMonoCallback(
      [&](bool forceMono) { requestedForceMono = forceMono; });
  xdrServer.setStartCallback([&]() {
    if (verboseLogging) {
      std::cout << "[XDR] tuner started by client\n";
    }
    connectTuner();
    dspRuntime.reset(fm_tuner::dsp::ResetReason::Start);
    tunerActive = true;
  });
  xdrServer.setStopCallback([&]() {
    if (verboseLogging) {
      std::cout << "[XDR] tuner stopped by client\n";
    }
    tunerActive = false;
    dspRuntime.reset(fm_tuner::dsp::ResetReason::Stop);
    audioOut.clearRealtimeQueue();
    disconnectTuner();
  });

  if (!xdrServer.start()) {
    std::cerr << "[XDR] failed to start XDR server\n";
  }

  if (autoStartTuner) {
    if (verboseLogging) {
      std::cout << "[AUTO] auto-starting tuner for local mode\n";
    }
    connectTuner();
    tunerActive = rtlConnected;
    if (!rtlConnected) {
      std::cerr << "[AUTO] warning: auto-start requested but " << tunerName()
                << " connect failed\n";
    }
  }

  std::atomic<bool> rdsStop(false);
  std::atomic<bool> rdsReset(false);
  std::mutex rdsQueueMutex;
  std::condition_variable rdsQueueCv;
  std::deque<std::vector<float>> rdsQueue;
  constexpr size_t RDS_QUEUE_LIMIT = 32;

  auto queueRdsBlock = [&](const float *samples, size_t count) {
    if (!samples || count == 0) {
      return;
    }

    {
      std::lock_guard<std::mutex> lock(rdsQueueMutex);
      if (rdsQueue.size() >= RDS_QUEUE_LIMIT) {
        // Keep continuity for decoder lock; drop newest block under overload.
        return;
      }
      rdsQueue.emplace_back();
      std::vector<float> &block = rdsQueue.back();
      block.resize(count);
      std::memcpy(block.data(), samples, count * sizeof(float));
    }
    rdsQueueCv.notify_one();
  };

  std::thread rdsThread([&]() {
    RDSDecoder rds(INPUT_RATE);
    while (!rdsStop.load()) {
      std::vector<float> block;
      bool doReset = false;

      {
        std::unique_lock<std::mutex> lock(rdsQueueMutex);
        rdsQueueCv.wait_for(lock, std::chrono::milliseconds(50), [&]() {
          return rdsStop.load() || rdsReset.load() || !rdsQueue.empty();
        });

        if (rdsStop.load()) {
          break;
        }

        doReset = rdsReset.exchange(false);
        if (!rdsQueue.empty()) {
          block = std::move(rdsQueue.front());
          rdsQueue.pop_front();
        }
      }

      if (doReset) {
        rds.reset();
      }

      if (!block.empty()) {
        rds.process(block.data(), block.size(), [&](const RDSGroup &group) {
          xdrServer.updateRDS(group.blockA, group.blockB, group.blockC,
                              group.blockD, group.errors);
        });
      }
    }
  });

  if (verboseLogging) {
    std::cout << "[APP] waiting for client connection on port " << xdrPort
              << "...\n";
    std::cout << "[APP] press Ctrl+C to stop.\n";
  }

  // Direct RTL-SDR async callback is configured to 16384 bytes (8192 IQ
  // samples). Read at tuner rate, then decimate to stable DSP-rate blocks.
  const size_t BUF_SAMPLES = dspRuntime.blockSize();
  const size_t SDR_BUF_SAMPLES = BUF_SAMPLES * iqDecimation;
  const auto noDataSleep = useDirectRtlSdr ? std::chrono::milliseconds(2)
                                           : std::chrono::milliseconds(10);
  const auto scanRetrySleep = useDirectRtlSdr ? std::chrono::milliseconds(2)
                                              : std::chrono::milliseconds(5);
  std::vector<uint8_t> iqBufferStorage(SDR_BUF_SAMPLES * 2, 0);
  std::vector<std::complex<float>> iqDecimatedComplexStorage(BUF_SAMPLES);
  std::vector<uint8_t> iqStagingStorage;
  if (iqDecimation > 1) {
    iqStagingStorage.reserve(SDR_BUF_SAMPLES * 4);
  }
  std::vector<float> demodBufferStorage(BUF_SAMPLES, 0.0f);
  std::vector<float> stereoLeftStorage(BUF_SAMPLES, 0.0f);
  std::vector<float> stereoRightStorage(BUF_SAMPLES, 0.0f);
  std::vector<float> audioLeftStorage(BUF_SAMPLES, 0.0f);
  std::vector<float> audioRightStorage(BUF_SAMPLES, 0.0f);
  uint8_t *iqBuffer = iqBufferStorage.data();
  std::complex<float> *iqDecimatedComplex = iqDecimatedComplexStorage.data();
  float *demodBuffer = demodBufferStorage.data();
  float *stereoLeft = stereoLeftStorage.data();
  float *stereoRight = stereoRightStorage.data();
  float *audioLeft = audioLeftStorage.data();
  float *audioRight = audioRightStorage.data();

  size_t retuneMuteSamplesRemaining = 0;
  size_t retuneMuteTotalSamples = 0;

  int consecutiveReadFailures = 0;
  bool scanActive = false;
  auto lastGainDown =
      std::chrono::steady_clock::now() - std::chrono::seconds(5);
  auto lastGainUp = std::chrono::steady_clock::now() - std::chrono::seconds(5);
  XDRServer::ScanConfig scanConfig;
  uint32_t scanRestoreFreqHz = requestedFrequencyHz.load();
  int scanRestoreBandwidthHz = appliedBandwidthHz;
  auto restoreAfterScan = [&]() {
    requestedBandwidthHz = scanRestoreBandwidthHz;
    pendingBandwidth = true;
    requestedFrequencyHz.store(scanRestoreFreqHz, std::memory_order_relaxed);
    pendingFrequency.store(true, std::memory_order_release);
    if (rtlConnected) {
      tunerSetFrequency(scanRestoreFreqHz);
    }
    dspRuntime.reset(fm_tuner::dsp::ResetReason::ScanRestore);
    retuneMuteSamplesRemaining = kRetuneMuteSamples;
    retuneMuteTotalSamples = kRetuneMuteSamples;
    rdsReset = true;
    {
      std::lock_guard<std::mutex> lock(rdsQueueMutex);
      rdsQueue.clear();
    }
    rdsQueueCv.notify_one();
  };
  while (g_running) {
    if (!tunerActive) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      continue;
    }

    XDRServer::ScanConfig newScanConfig;
    if (xdrServer.consumeScanStart(newScanConfig)) {
      scanConfig = newScanConfig;
      scanActive = true;
      scanRestoreFreqHz = requestedFrequencyHz.load();
      scanRestoreBandwidthHz = appliedBandwidthHz;
      if (scanConfig.bandwidthHz > 0) {
        requestedBandwidthHz = scanConfig.bandwidthHz;
        pendingBandwidth = true;
      }
      if (verboseLogging) {
        std::cout << "[SCAN] start "
                  << "from=" << scanConfig.startKHz
                  << " to=" << scanConfig.stopKHz
                  << " step=" << scanConfig.stepKHz
                  << " bw=" << scanConfig.bandwidthHz << " mode="
                  << (scanConfig.continuous ? "continuous" : "single") << "\n";
      }
    }
    if (xdrServer.consumeScanCancel()) {
      const bool wasActive = scanActive;
      scanActive = false;
      if (verboseLogging) {
        std::cout << "[SCAN] cancel requested\n";
      }
      if (wasActive && rtlConnected) {
        restoreAfterScan();
      }
    }

    if (rtlConnected && pendingFrequency.exchange(false)) {
      tunerSetFrequency(requestedFrequencyHz.load());
      audioOut.clearRealtimeQueue();
      // Clear pilot/stereo state on retune to avoid carrying lock across
      // stations.
      dspRuntime.reset(fm_tuner::dsp::ResetReason::Retune);
      retuneMuteSamplesRemaining = kRetuneMuteSamples;
      retuneMuteTotalSamples = kRetuneMuteSamples;
      rdsReset = true;
      {
        std::lock_guard<std::mutex> lock(rdsQueueMutex);
        rdsQueue.clear();
      }
      rdsQueueCv.notify_one();
    }
    const bool gainChanged = pendingGain.exchange(false);
    const bool agcChanged = pendingAGC.exchange(false);
    const bool bandwidthChanged = pendingBandwidth.exchange(false);
    if (rtlConnected && (gainChanged || agcChanged)) {
      applyRtlGainAndAgc(
          (gainChanged && agcChanged)
              ? "runtime/update(A+G)"
              : (agcChanged ? "runtime/update(A)" : "runtime/update(G)"));
    }
    if (bandwidthChanged) {
      const int targetBandwidthHz = requestedBandwidthHz.load();
      if (targetBandwidthHz != appliedBandwidthHz) {
        demod.setBandwidthHz(targetBandwidthHz);
        if (verboseLogging) {
          std::cout << "[BW] applied W" << targetBandwidthHz << " (previous W"
                    << appliedBandwidthHz << ")\n";
        }
        appliedBandwidthHz = targetBandwidthHz;
      }
    }

    if (scanActive && rtlConnected) {
      const int startKHz = std::min(scanConfig.startKHz, scanConfig.stopKHz);
      const int stopKHz = std::max(scanConfig.startKHz, scanConfig.stopKHz);
      const int stepKHz = std::max(5, scanConfig.stepKHz);

      std::ostringstream scanLine;
      bool firstPoint = true;
      for (int f = startKHz; f <= stopKHz; f += stepKHz) {
        if (!g_running || xdrServer.consumeScanCancel()) {
          scanActive = false;
          break;
        }

        tunerSetFrequency(static_cast<uint32_t>(f) * 1000U);

        double powerAccum = 0.0;
        int validReads = 0;
        for (int avg = 0; avg < 3; avg++) {
          size_t samples = 0;
          for (int retries = 0; retries < 2 && samples == 0; retries++) {
            samples = tunerReadIQ(iqBuffer, SDR_BUF_SAMPLES);
            if (samples == 0) {
              std::this_thread::sleep_for(scanRetrySleep);
            }
          }
          if (samples > 0) {
            writeIqCapture(iqBuffer, samples);
            const SignalLevelResult signal = computeSignalLevel(
                iqBuffer, samples, effectiveAppliedGainDb(),
                kSignalGainCompFactor, config.sdr.signal_bias_db,
                config.sdr.signal_floor_dbfs, config.sdr.signal_ceil_dbfs);
            powerAccum += static_cast<double>(signal.level120);
            validReads++;
          }
        }
        if (validReads == 0) {
          continue;
        }
        const float rfLevel =
            static_cast<float>(powerAccum / static_cast<double>(validReads));

        if (!firstPoint) {
          scanLine << ",";
        }
        firstPoint = false;
        scanLine << f << "=" << std::fixed << std::setprecision(1) << rfLevel;
      }

      if (!scanLine.str().empty()) {
        xdrServer.pushScanLine(scanLine.str());
      }

      if (!scanConfig.continuous || !scanActive) {
        scanActive = false;
        restoreAfterScan();
      }
      continue;
    }

    int targetDeemphasis = requestedDeemphasis.load();
    if (targetDeemphasis != appliedDeemphasis) {
      if (targetDeemphasis == 0) {
        afPost.setDeemphasis(50);
        demod.setDeemphasis(50);
      } else if (targetDeemphasis == 1) {
        afPost.setDeemphasis(75);
        demod.setDeemphasis(75);
      } else {
        afPost.setDeemphasis(0);
        demod.setDeemphasis(0);
      }
      appliedDeemphasis = targetDeemphasis;
    }

    bool targetForceMono = requestedForceMono.load();
    if (targetForceMono != appliedForceMono) {
      appliedForceMono = targetForceMono;
    }

    size_t samples = tunerReadIQ(iqBuffer, SDR_BUF_SAMPLES);
    if (samples == 0) {
      consecutiveReadFailures++;
      if (autoReconnect && rtlConnected && consecutiveReadFailures >= 20) {
        std::cerr << "[SDR] no IQ data, reconnecting...\n";
        disconnectTuner();
        connectTuner();
        consecutiveReadFailures = 0;
      }
      std::this_thread::sleep_for(noDataSleep);
      continue;
    }
    writeIqCapture(iqBuffer, samples);
    if (verboseLogging && samples < SDR_BUF_SAMPLES) {
      static uint32_t shortIqReadCount = 0;
      const uint32_t count = ++shortIqReadCount;
      if (count <= 5 || (count % 50) == 0) {
        std::cerr << "[SDR] short IQ read (" << count << "): " << samples << "/"
                  << SDR_BUF_SAMPLES << " samples\n";
      }
    }
    consecutiveReadFailures = 0;

    // RF-domain level estimate from raw IQ before demodulation.
    const SignalLevelResult signal = computeSignalLevel(
        iqBuffer, samples, effectiveAppliedGainDb(), kSignalGainCompFactor,
        config.sdr.signal_bias_db, config.sdr.signal_floor_dbfs,
        config.sdr.signal_ceil_dbfs);
    const double clipRatio =
        std::max(signal.hardClipRatio, signal.nearClipRatio);
    const float rfLevelFiltered =
        smoothSignalLevel(signal.level120, rfLevelSmoother);
    if (verboseLogging) {
      static uint32_t signalLogCount = 0;
      const uint32_t count = ++signalLogCount;
      if (count <= 5 || (count % 100) == 0) {
        std::cout << "[SIG] dbfs=" << std::fixed << std::setprecision(2)
                  << signal.dbfs << " compensated=" << signal.compensatedDbfs
                  << " level=" << std::setprecision(1) << signal.level120
                  << " filtered=" << rfLevelFiltered
                  << " clip=" << std::setprecision(4) << clipRatio << "\n";
      }
    }

    if (!useSdrppGainStrategy && gain < 0 && !isImsAgcEnabled()) {
      const auto now = std::chrono::steady_clock::now();
      const bool overload = (clipRatio > 0.0200) || (signal.dbfs > -5.0);
      const bool weak = (clipRatio < 0.0005) &&
                        (signal.compensatedDbfs < -47.0) &&
                        (rfLevelFiltered < 35.0f);

      if (overload && (now - lastGainDown) >= std::chrono::milliseconds(900)) {
        const int current = std::clamp(requestedAGCMode.load(), 0, 3);
        if (current < 3) {
          requestedAGCMode = current + 1;
          pendingAGC = true;
          lastGainDown = now;
          if (verboseLogging) {
            std::cout << "[GAIN] clip-protect: A" << current << " -> A"
                      << (current + 1) << " (dbfs=" << std::fixed
                      << std::setprecision(2) << signal.dbfs
                      << ", clip=" << std::setprecision(4) << clipRatio
                      << ")\n";
          }
        }
      } else if (weak &&
                 (now - lastGainUp) >= std::chrono::milliseconds(4000)) {
        const int current = std::clamp(requestedAGCMode.load(), 0, 3);
        if (current > 0) {
          requestedAGCMode = current - 1;
          pendingAGC = true;
          lastGainUp = now;
          if (verboseLogging) {
            std::cout << "[GAIN] sensitivity-up: A" << current << " -> A"
                      << (current - 1) << " (comp=" << std::fixed
                      << std::setprecision(2) << signal.compensatedDbfs
                      << ", clip=" << std::setprecision(4) << clipRatio
                      << ")\n";
          }
        }
      }
    }

    const bool effectiveForceMono = targetForceMono;
    if (effectiveForceMono != appliedEffectiveForceMono) {
      stereo.setForceMono(effectiveForceMono);
      appliedEffectiveForceMono = effectiveForceMono;
    }

    size_t outSamples = 0;
    bool stereoDetected = false;
    int pilotTenthsKHz = 0;

    const uint8_t *iqForDemod = iqBuffer;
    const std::complex<float> *iqForDemodComplex = nullptr;
    size_t demodSamples = samples;
    if (iqDecimation > 1) {
      const size_t appendedBytes = samples * 2;
      const size_t oldSize = iqStagingStorage.size();
      iqStagingStorage.resize(oldSize + appendedBytes);
      std::memcpy(iqStagingStorage.data() + oldSize, iqBuffer, appendedBytes);

      const size_t availableSamples = iqStagingStorage.size() / 2;
      if (availableSamples < SDR_BUF_SAMPLES) {
        continue;
      }

      demodSamples =
          iqDecimator.executeComplex(iqStagingStorage.data(), SDR_BUF_SAMPLES,
                                     iqDecimatedComplex, BUF_SAMPLES);
      iqForDemodComplex = iqDecimatedComplex;
      const size_t consumedBytes = SDR_BUF_SAMPLES * 2;
      const size_t remainingBytes = iqStagingStorage.size() - consumedBytes;
      if (remainingBytes > 0) {
        std::memmove(iqStagingStorage.data(),
                     iqStagingStorage.data() + consumedBytes, remainingBytes);
      }
      iqStagingStorage.resize(remainingBytes);
      if (demodSamples == 0) {
        std::this_thread::sleep_for(noDataSleep);
        continue;
      }
    }

    if (!config.processing.stereo) {
      outSamples =
          (iqForDemodComplex != nullptr)
              ? demod.processSplitComplex(iqForDemodComplex, demodBuffer,
                                          audioLeft, demodSamples)
              : demod.processSplit(iqForDemod, demodBuffer, audioLeft,
                                   demodSamples);
      queueRdsBlock(demodBuffer, demodSamples);
      for (size_t i = 0; i < outSamples; i++) {
        const float mono = audioLeft[i] * 0.5f;
        audioLeft[i] = mono;
        audioRight[i] = mono;
      }
    } else {
      // Feed RDS with full-rate discriminator MPX (includes 57 kHz RDS
      // subcarrier). redsea expects composite MPX, not post-audio-filtered
      // baseband.
      if (iqForDemodComplex != nullptr) {
        demod.processSplitComplex(iqForDemodComplex, demodBuffer, nullptr,
                                  demodSamples);
      } else {
        demod.processSplit(iqForDemod, demodBuffer, nullptr, demodSamples);
      }
      queueRdsBlock(demodBuffer, demodSamples);
      const size_t stereoSamples = stereo.processAudio(
          demodBuffer, stereoLeft, stereoRight, demodSamples);
      outSamples = afPost.process(stereoLeft, stereoRight, stereoSamples,
                                  audioLeft, audioRight, BUF_SAMPLES);
      stereoDetected = stereo.isStereo();
      pilotTenthsKHz = stereo.getPilotLevelTenthsKHz();
    }
    const bool stereoIndicator =
        stereoDetected || (effectiveForceMono && config.processing.stereo &&
                           pilotTenthsKHz >= 20);
    xdrServer.updateSignal(rfLevelFiltered, stereoIndicator, effectiveForceMono,
                           -1, -1);
    xdrServer.updatePilot(pilotTenthsKHz);

    for (size_t i = 0; i < outSamples; i++) {
      audioLeft[i] = std::clamp(audioLeft[i], -1.0f, 1.0f);
      audioRight[i] = std::clamp(audioRight[i], -1.0f, 1.0f);
    }

    if (retuneMuteSamplesRemaining > 0 && outSamples > 0) {
      const size_t muteCount = std::min(outSamples, retuneMuteSamplesRemaining);
      const size_t alreadyMuted =
          (retuneMuteTotalSamples > retuneMuteSamplesRemaining)
              ? (retuneMuteTotalSamples - retuneMuteSamplesRemaining)
              : 0;
      const size_t fadeSamples = std::max<size_t>(
          1, std::min(static_cast<size_t>(OUTPUT_RATE / 200),
                      retuneMuteTotalSamples / 2)); // up to ~5 ms
      for (size_t i = 0; i < muteCount; i++) {
        const size_t idx = alreadyMuted + i;
        float gain = 0.0f;
        if (idx < fadeSamples) {
          gain = 1.0f -
                 (static_cast<float>(idx) / static_cast<float>(fadeSamples));
        } else if (idx >= (retuneMuteTotalSamples - fadeSamples)) {
          const size_t tail = retuneMuteTotalSamples - idx;
          gain = static_cast<float>(tail) / static_cast<float>(fadeSamples);
        }
        gain = std::clamp(gain, 0.0f, 1.0f);
        audioLeft[i] *= gain;
        audioRight[i] *= gain;
      }
      retuneMuteSamplesRemaining -= muteCount;
      if (retuneMuteSamplesRemaining == 0) {
        retuneMuteTotalSamples = 0;
      }
    }

    if (outSamples > 0) {
      audioOut.write(audioLeft, audioRight, outSamples);
    }
  }

  rdsStop = true;
  rdsQueueCv.notify_all();
  if (rdsThread.joinable()) {
    rdsThread.join();
  }

  audioOut.shutdown();
  if (iqHandle) {
    std::fclose(iqHandle);
    iqHandle = nullptr;
  }
  xdrServer.stop();
  tunerDisconnect();

  std::cout << "[APP] shutdown complete.\n";
  return 0;
}
