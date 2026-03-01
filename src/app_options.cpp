#include "app_options.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <iostream>
#include <string>

#include "audio_output.h"

namespace {

bool parseSourceOption(std::string value, std::string &outSource) {
  std::transform(
      value.begin(), value.end(), value.begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (value == "rtl_tcp" || value == "tcp") {
    outSource = "rtl_tcp";
    return true;
  }
  if (value == "rtl_sdr" || value == "sdr") {
    outSource = "rtl_sdr";
    return true;
  }
  return false;
}

} // namespace

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

AppParseResult parseAppOptions(int argc, char *argv[], int inputRate) {
  AppParseResult result;
  AppOptions &opts = result.options;

  for (int i = 1; i < argc; i++) {
    const std::string arg = argv[i];
    if (arg == "-c" && i + 1 < argc) {
      opts.configPath = argv[++i];
      continue;
    }
    static constexpr const char *kConfigPrefix = "--config=";
    if (arg.rfind(kConfigPrefix, 0) == 0) {
      opts.configPath = arg.substr(std::strlen(kConfigPrefix));
      continue;
    }
    if (arg == "--config" && i + 1 < argc) {
      opts.configPath = argv[++i];
      continue;
    }
  }

  opts.config.loadDefaults();
  if (!opts.configPath.empty() && !opts.config.loadFromFile(opts.configPath)) {
    result.outcome = AppParseOutcome::ExitFailure;
    return result;
  }

  opts.verboseLogging = opts.config.debug.log_level > 0;
  opts.tcpHost = opts.config.rtl_tcp.host;
  opts.tcpPort = opts.config.rtl_tcp.port;
  opts.iqSampleRate = opts.config.rtl_tcp.sample_rate;
  opts.tunerSource = opts.config.tuner.source;
  opts.rtlDeviceIndex = opts.config.tuner.rtl_device;
  opts.freqKHz = opts.config.tuner.default_freq;
  opts.gain = opts.config.sdr.rtl_gain_db;
  opts.enableSpeaker = opts.config.audio.enable_audio;
  opts.xdrPassword = opts.config.xdr.password;
  opts.xdrGuestMode = opts.config.xdr.guest_mode;
  opts.xdrPort = opts.config.xdr.port;
  opts.autoReconnect = opts.config.reconnection.auto_reconnect;
  opts.lowLatencyIq = opts.config.sdr.low_latency_iq;

  if (!parseSourceOption(opts.tunerSource, opts.tunerSource)) {
    std::cerr << "[Config] invalid tuner.source: " << opts.config.tuner.source
              << " (expected rtl_tcp or rtl_sdr), using rtl_sdr\n";
    opts.tunerSource = "rtl_sdr";
  }

  auto parseTcpOption = [&](const std::string &value) -> bool {
    size_t colon = value.find(':');
    if (colon != std::string::npos) {
      opts.tcpHost = value.substr(0, colon);
      try {
        const int parsedPort = std::stoi(value.substr(colon + 1));
        if (parsedPort < 1 || parsedPort > 65535) {
          std::cerr << "[CLI] invalid tcp port: " << parsedPort << "\n";
          return false;
        }
        opts.tcpPort = static_cast<uint16_t>(parsedPort);
      } catch (...) {
        std::cerr << "[CLI] invalid --tcp value: " << value << "\n";
        return false;
      }
    } else {
      opts.tcpHost = value;
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
      opts.freqKHz = static_cast<uint32_t>(parsedFreq);
      return true;
    } catch (...) {
      std::cerr << "[CLI] invalid --freq value: " << value << "\n";
      return false;
    }
  };

  auto parseDeviceIndexOption = [&](const std::string &value) -> bool {
    try {
      const int parsed = std::stoi(value);
      if (parsed < 0) {
        std::cerr << "[CLI] invalid --rtl-device value: " << value << "\n";
        return false;
      }
      opts.rtlDeviceIndex = static_cast<uint32_t>(parsed);
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
      result.outcome = AppParseOutcome::ExitSuccess;
      return result;
    }
    if (arg == "-s" || arg == "--audio") {
      opts.enableSpeaker = true;
      continue;
    }
    if (arg == "--low-latency-iq") {
      opts.lowLatencyIq = true;
      continue;
    }
    if (arg == "--no-low-latency-iq") {
      opts.lowLatencyIq = false;
      continue;
    }
    if (arg == "-G" || arg == "--guest") {
      opts.xdrGuestMode = true;
      continue;
    }
    if (arg == "-l" || arg == "--list-audio") {
      if (!AudioOutput::listDevices()) {
        result.outcome = AppParseOutcome::ExitFailure;
        return result;
      }
      result.outcome = AppParseOutcome::ExitSuccess;
      return result;
    }

    if (arg == "-c" || arg == "--config" || arg.rfind("--config=", 0) == 0) {
      const std::string value = readValue(i, arg, "config");
      if (value.empty()) {
        std::cerr << "[CLI] missing value for --config\n";
        result.outcome = AppParseOutcome::ExitFailure;
        return result;
      }
      opts.configPath = value;
      continue;
    }
    if (arg == "-t" || arg == "--tcp" || arg.rfind("--tcp=", 0) == 0) {
      const std::string value = readValue(i, arg, "tcp");
      if (value.empty() || !parseTcpOption(value)) {
        result.outcome = AppParseOutcome::ExitFailure;
        return result;
      }
      continue;
    }
    if (arg == "--iq-rate" || arg.rfind("--iq-rate=", 0) == 0) {
      const std::string value = readValue(i, arg, "iq-rate");
      try {
        const int parsed = std::stoi(value);
        if (parsed == 256000 || parsed == 1024000 || parsed == 2048000) {
          opts.iqSampleRate = static_cast<uint32_t>(parsed);
        } else {
          std::cerr << "[CLI] invalid --iq-rate value: " << value
                    << " (expected 256000, 1024000, or 2048000)\n";
          result.outcome = AppParseOutcome::ExitFailure;
          return result;
        }
      } catch (...) {
        std::cerr << "[CLI] invalid --iq-rate value: " << value << "\n";
        result.outcome = AppParseOutcome::ExitFailure;
        return result;
      }
      continue;
    }
    if (arg == "--source" || arg.rfind("--source=", 0) == 0) {
      const std::string value = readValue(i, arg, "source");
      if (value.empty() || !parseSourceOption(value, opts.tunerSource)) {
        std::cerr << "[CLI] invalid source value: " << value
                  << " (expected rtl_tcp or rtl_sdr)\n";
        result.outcome = AppParseOutcome::ExitFailure;
        return result;
      }
      continue;
    }
    if (arg == "--rtl-device" || arg.rfind("--rtl-device=", 0) == 0) {
      const std::string value = readValue(i, arg, "rtl-device");
      if (value.empty() || !parseDeviceIndexOption(value)) {
        result.outcome = AppParseOutcome::ExitFailure;
        return result;
      }
      continue;
    }
    if (arg == "-f" || arg == "--freq" || arg.rfind("--freq=", 0) == 0) {
      const std::string value = readValue(i, arg, "freq");
      if (value.empty() || !parseUIntFreqKHz(value)) {
        result.outcome = AppParseOutcome::ExitFailure;
        return result;
      }
      continue;
    }
    if (arg == "-g" || arg == "--gain" || arg.rfind("--gain=", 0) == 0) {
      const std::string value = readValue(i, arg, "gain");
      if (value.empty() || !parseIntOption("gain", value, opts.gain)) {
        result.outcome = AppParseOutcome::ExitFailure;
        return result;
      }
      continue;
    }
    if (arg == "-w" || arg == "--wav" || arg.rfind("--wav=", 0) == 0) {
      const std::string value = readValue(i, arg, "wav");
      if (value.empty()) {
        std::cerr << "[CLI] missing value for --wav\n";
        result.outcome = AppParseOutcome::ExitFailure;
        return result;
      }
      opts.wavFile = value;
      continue;
    }
    if (arg == "-i" || arg == "--iq" || arg.rfind("--iq=", 0) == 0) {
      const std::string value = readValue(i, arg, "iq");
      if (value.empty()) {
        std::cerr << "[CLI] missing value for --iq\n";
        result.outcome = AppParseOutcome::ExitFailure;
        return result;
      }
      opts.iqFile = value;
      continue;
    }
    if (arg == "-d" || arg == "--device" || arg.rfind("--device=", 0) == 0) {
      const std::string value = readValue(i, arg, "device");
      if (value.empty()) {
        std::cerr << "[CLI] missing value for --device\n";
        result.outcome = AppParseOutcome::ExitFailure;
        return result;
      }
      opts.audioDevice = value;
      continue;
    }
    if (arg == "-P" || arg == "--password" || arg.rfind("--password=", 0) == 0) {
      const std::string value = readValue(i, arg, "password");
      if (value.empty()) {
        std::cerr << "[CLI] missing value for --password\n";
        result.outcome = AppParseOutcome::ExitFailure;
        return result;
      }
      opts.xdrPassword = value;
      continue;
    }

    std::cerr << "[CLI] unknown option: " << arg << "\n";
    printUsage(argv[0]);
    result.outcome = AppParseOutcome::ExitFailure;
    return result;
  }

  if (opts.wavFile.empty() && opts.iqFile.empty() && !opts.enableSpeaker) {
    std::cerr << "[CLI] error: must specify at least one output: -w (wav), -i "
                 "(iq), or -s (audio)\n";
    printUsage(argv[0]);
    result.outcome = AppParseOutcome::ExitFailure;
    return result;
  }

  if (!(opts.iqSampleRate == 256000 || opts.iqSampleRate == 1024000 ||
        opts.iqSampleRate == 2048000)) {
    std::cerr << "[SDR] unsupported iq sample rate: " << opts.iqSampleRate
              << " (expected 256000, 1024000, or 2048000)\n";
    result.outcome = AppParseOutcome::ExitFailure;
    return result;
  }
  const size_t iqDecimation = static_cast<size_t>(opts.iqSampleRate / inputRate);
  if (iqDecimation == 0 || (opts.iqSampleRate % inputRate) != 0) {
    std::cerr << "[SDR] iq sample rate must be an integer multiple of "
              << inputRate << "\n";
    result.outcome = AppParseOutcome::ExitFailure;
    return result;
  }

  result.outcome = AppParseOutcome::Run;
  return result;
}
