#include <iostream>
#include <csignal>
#include <atomic>
#include <cctype>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <vector>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <array>
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
#include <immintrin.h>
#endif
#if defined(__aarch64__) || defined(__arm__) || defined(_M_ARM64)
#include <arm_neon.h>
#endif

#include "rtl_tcp_client.h"
#include "fm_demod.h"
#include "stereo_decoder.h"
#include "af_post_processor.h"
#include "cpu_features.h"
#include "rds_decoder.h"
#include "xdr_server.h"
#include "audio_output.h"
#include "config.h"
#include "rtl_sdr_device.h"
#include "dsp/runtime.h"

static std::atomic<bool> g_running(true);

namespace {
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
#if defined(__has_attribute)
#if __has_attribute(target)
#define FMTUNER_MAIN_HAS_AVX2_KERNEL 1
#define FMTUNER_MAIN_AVX2_TARGET __attribute__((target("avx2,fma")))
#endif
#elif defined(__GNUC__)
#define FMTUNER_MAIN_HAS_AVX2_KERNEL 1
#define FMTUNER_MAIN_AVX2_TARGET __attribute__((target("avx2,fma")))
#endif
#endif
#ifndef FMTUNER_MAIN_HAS_AVX2_KERNEL
#define FMTUNER_MAIN_HAS_AVX2_KERNEL 0
#define FMTUNER_MAIN_AVX2_TARGET
#endif

double computeNormalizedIqPowerSumScalar(const uint8_t* iq, size_t count, const float* lut) {
    double powerSum = 0.0;
    size_t i = 0;
    for (; i + 4 <= count; i += 4) {
        powerSum += lut[iq[i]];
        powerSum += lut[iq[i + 1]];
        powerSum += lut[iq[i + 2]];
        powerSum += lut[iq[i + 3]];
    }
    for (; i < count; i++) {
        powerSum += lut[iq[i]];
    }
    return powerSum;
}

#if FMTUNER_MAIN_HAS_AVX2_KERNEL
FMTUNER_MAIN_AVX2_TARGET double computeNormalizedIqPowerSumAvx2(const uint8_t* iq, size_t count, const float* lut) {
    __m256 acc = _mm256_setzero_ps();
    size_t i = 0;
    for (; i + 8 <= count; i += 8) {
        const __m128i b8 = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(iq + i));
        const __m256i idx = _mm256_cvtepu8_epi32(b8);
        const __m256 vals = _mm256_i32gather_ps(lut, idx, 4);
        acc = _mm256_add_ps(acc, vals);
    }
    alignas(32) float lanes[8];
    _mm256_store_ps(lanes, acc);
    double powerSum = lanes[0] + lanes[1] + lanes[2] + lanes[3] + lanes[4] + lanes[5] + lanes[6] + lanes[7];
    for (; i < count; i++) {
        powerSum += lut[iq[i]];
    }
    return powerSum;
}
#endif
#if defined(__aarch64__) || defined(__arm__) || defined(_M_ARM64)
double computeNormalizedIqPowerSumNeon(const uint8_t* iq, size_t count) {
    const float32x4_t vMid = vdupq_n_f32(127.0f);
    const float32x4_t vInv = vdupq_n_f32(1.0f / 127.5f);
    float32x4_t acc = vdupq_n_f32(0.0f);
    size_t i = 0;
    for (; i + 16 <= count; i += 16) {
        const uint8x16_t v = vld1q_u8(iq + i);
        const uint16x8_t lo16 = vmovl_u8(vget_low_u8(v));
        const uint16x8_t hi16 = vmovl_u8(vget_high_u8(v));
        const uint32x4_t lo32a = vmovl_u16(vget_low_u16(lo16));
        const uint32x4_t lo32b = vmovl_u16(vget_high_u16(lo16));
        const uint32x4_t hi32a = vmovl_u16(vget_low_u16(hi16));
        const uint32x4_t hi32b = vmovl_u16(vget_high_u16(hi16));
        float32x4_t f0 = vcvtq_f32_u32(lo32a);
        float32x4_t f1 = vcvtq_f32_u32(lo32b);
        float32x4_t f2 = vcvtq_f32_u32(hi32a);
        float32x4_t f3 = vcvtq_f32_u32(hi32b);
        f0 = vmulq_f32(vsubq_f32(f0, vMid), vInv);
        f1 = vmulq_f32(vsubq_f32(f1, vMid), vInv);
        f2 = vmulq_f32(vsubq_f32(f2, vMid), vInv);
        f3 = vmulq_f32(vsubq_f32(f3, vMid), vInv);
        acc = vaddq_f32(acc, vmulq_f32(f0, f0));
        acc = vaddq_f32(acc, vmulq_f32(f1, f1));
        acc = vaddq_f32(acc, vmulq_f32(f2, f2));
        acc = vaddq_f32(acc, vmulq_f32(f3, f3));
    }
#if defined(__aarch64__)
    double powerSum = static_cast<double>(vaddvq_f32(acc));
#else
    float tmp[4];
    vst1q_f32(tmp, acc);
    double powerSum = tmp[0] + tmp[1] + tmp[2] + tmp[3];
#endif
    for (; i < count; i++) {
        const float norm = (static_cast<float>(iq[i]) - 127.0f) * (1.0f / 127.5f);
        powerSum += static_cast<double>(norm * norm);
    }
    return powerSum;
}
#endif

double computeNormalizedIqPowerSum(const uint8_t* iq, size_t samples) {
    static const std::array<float, 256> kNormSqLut = []() {
        std::array<float, 256> lut{};
        for (int v = 0; v < 256; v++) {
            const float norm = (static_cast<float>(v) - 127.0f) / 127.5f;
            lut[static_cast<size_t>(v)] = norm * norm;
        }
        return lut;
    }();
    const size_t count = samples * 2;
    static const CPUFeatures cpu = detectCPUFeatures();
#if FMTUNER_MAIN_HAS_AVX2_KERNEL
    if (cpu.avx2 && cpu.fma) {
        return computeNormalizedIqPowerSumAvx2(iq, count, kNormSqLut.data());
    }
#endif
#if defined(__aarch64__) || defined(__arm__) || defined(_M_ARM64)
    if (cpu.neon) {
        return computeNormalizedIqPowerSumNeon(iq, count);
    }
#endif
    return computeNormalizedIqPowerSumScalar(iq, count, kNormSqLut.data());
}
}  // namespace

void signalHandler(int) {
    g_running = false;
}

void printUsage(const char* prog) {
    std::cout << "Usage: " << prog << " [options]\n"
              << "Options:\n"
              << "  -c, --config <file>    INI config file\n"
              << "  -t, --tcp <host:port>  rtl_tcp server address (default: localhost:1234)\n"
              << "      --source <name>    Tuner source: rtl_tcp or rtl_sdr (default: rtl_sdr)\n"
              << "      --rtl-device <id>  RTL-SDR device index for --source rtl_sdr (default: 0)\n"
              << "  -f, --freq <khz>      Frequency in kHz (default: 88600)\n"
              << "  -g, --gain <db>       RTL-SDR gain in dB (default: auto)\n"
              << "  -w, --wav <file>      Output WAV file\n"
              << "  -i, --iq <file>       Capture raw IQ bytes to file\n"
              << "  -s, --audio           Enable audio output\n"
              << "  -l, --list-audio      List available audio output devices\n"
              << "  -d, --device <id>     Audio output device (index or name)\n"
              << "  -P, --password <pwd>   XDR server password\n"
              << "  -G, --guest            Enable guest mode (no password required)\n"
              << "  -h, --help             Show this help\n";
}

int main(int argc, char* argv[]) {
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
        static constexpr const char* kConfigPrefix = "--config=";
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
        std::cout << "[Config] processing.dsp_block_samples=" << config.processing.dsp_block_samples << "\n";
        std::cout << "[Config] processing.w0_bandwidth_hz=" << config.processing.w0_bandwidth_hz << "\n";
        std::cout << "[Config] processing.stereo_blend='" << config.processing.stereo_blend << "'\n";
    }
    std::string tcpHost = config.rtl_tcp.host;
    uint16_t tcpPort = config.rtl_tcp.port;
    std::string tunerSource = config.tuner.source;
    uint32_t rtlDeviceIndex = config.tuner.rtl_device;
    uint32_t freqKHz = config.tuner.default_freq;
    int gain = config.sdr.rtl_gain_db;
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

    auto parseTcpOption = [&](const std::string& value) -> bool {
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

    auto parseIntOption = [&](const std::string& name, const std::string& value, int& out) -> bool {
        try {
            out = std::stoi(value);
            return true;
        } catch (...) {
            std::cerr << "[CLI] invalid --" << name << " value: " << value << "\n";
            return false;
        }
    };

    auto parseUIntFreqKHz = [&](const std::string& value) -> bool {
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
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        if (value == "rtl_tcp" || value == "tcp") {
            tunerSource = "rtl_tcp";
            return true;
        }
        if (value == "rtl_sdr" || value == "sdr") {
            tunerSource = "rtl_sdr";
            return true;
        }
        std::cerr << "[CLI] invalid source value: " << value << " (expected rtl_tcp or rtl_sdr)\n";
        return false;
    };

    if (!parseSourceOption(tunerSource)) {
        std::cerr << "[Config] invalid tuner.source: " << config.tuner.source
                  << " (expected rtl_tcp or rtl_sdr), using rtl_sdr\n";
        tunerSource = "rtl_sdr";
    }

    auto parseDeviceIndexOption = [&](const std::string& value) -> bool {
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

    auto readValue = [&](int& index, const std::string& current, const std::string& longName) -> std::string {
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
        if (arg == "-P" || arg == "--password" || arg.rfind("--password=", 0) == 0) {
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
        std::cerr << "[CLI] error: must specify at least one output: -w (wav), -i (iq), or -s (audio)\n";
        printUsage(argv[0]);
        return 1;
    }

    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    RTLTCPClient rtlTcpClient(tcpHost, tcpPort);
    RTLSDRDevice rtlSdrDevice(rtlDeviceIndex);
    const bool useDirectRtlSdr = (tunerSource == "rtl_sdr");
    bool rtlConnected = false;
    std::atomic<uint32_t> requestedFrequencyHz(freqKHz * 1000);
    std::atomic<int> requestedCustomGain(defaultCustomGainFlags);
    std::atomic<int> requestedAGCMode(std::clamp(config.processing.agc_mode, 0, 3));
    std::atomic<int> requestedBandwidthHz(0);
    std::atomic<int> requestedVolume(100);
    std::atomic<int> requestedDeemphasis(std::clamp(config.tuner.deemphasis, 0, 2));
    std::atomic<bool> requestedForceMono(false);
    std::atomic<bool> pendingFrequency(false);
    std::atomic<bool> pendingGain(false);
    std::atomic<bool> pendingAGC(false);
    std::atomic<bool> pendingBandwidth(false);

    auto tunerName = [&]() -> const char* {
        return useDirectRtlSdr ? "rtl_sdr" : "rtl_tcp";
    };
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
        return useDirectRtlSdr ? rtlSdrDevice.setFrequency(freqHz) : rtlTcpClient.setFrequency(freqHz);
    };
    auto tunerSetSampleRate = [&](uint32_t sampleRate) -> bool {
        return useDirectRtlSdr ? rtlSdrDevice.setSampleRate(sampleRate) : rtlTcpClient.setSampleRate(sampleRate);
    };
    auto tunerSetGainMode = [&](bool manual) -> bool {
        return useDirectRtlSdr ? rtlSdrDevice.setGainMode(manual) : rtlTcpClient.setGainMode(manual);
    };
    auto tunerSetGain = [&](uint32_t gainTenthsDb) -> bool {
        return useDirectRtlSdr ? rtlSdrDevice.setGain(gainTenthsDb) : rtlTcpClient.setGain(gainTenthsDb);
    };
    auto tunerSetAgc = [&](bool enable) -> bool {
        return useDirectRtlSdr ? rtlSdrDevice.setAGC(enable) : rtlTcpClient.setAGC(enable);
    };
    auto tunerReadIQ = [&](uint8_t* buffer, size_t maxSamples) -> size_t {
        return useDirectRtlSdr ? rtlSdrDevice.readIQ(buffer, maxSamples) : rtlTcpClient.readIQ(buffer, maxSamples);
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

    auto applyRtlGainAndAgc = [&](const char* reason) {
        if (!rtlConnected) {
            return;
        }
        if (useSdrppGainStrategy) {
            bool okGainMode = false;
            bool okAgc = false;
            bool okGain = true;
            if (gain >= 0) {
                okGainMode = tunerSetGainMode(true);
                okGain = tunerSetGain(static_cast<uint32_t>(std::clamp(gain, 0, 49) * 10));
            } else {
                okGainMode = tunerSetGainMode(true);
                okGain = tunerSetGain(static_cast<uint32_t>(config.sdr.sdrpp_rtl_agc_gain_db * 10));
            }
            okAgc = tunerSetAgc(config.sdr.sdrpp_rtl_agc);
            if (verboseLogging) {
                std::cout << "[SDR] " << reason
                          << " strategy=sdrpp"
                          << " tuner_agc=" << ((gain < 0) ? 1 : 0)
                          << " rtl_agc=" << (config.sdr.sdrpp_rtl_agc ? 1 : 0)
                          << " if_gain=" << ((gain >= 0) ? std::clamp(gain, 0, 49) : config.sdr.sdrpp_rtl_agc_gain_db)
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
            std::cout << "[SDR] " << reason
                      << " A" << agcMode
                      << " G" << formatCustomGain(custom)
                      << " (rf=" << rf << ",if=" << ifv << ")"
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
                std::cout << "[SDR] connecting to rtl_sdr device " << rtlDeviceIndex << "...\n";
            } else {
                std::cout << "[SDR] connecting to rtl_tcp at " << tcpHost << ":" << tcpPort << "...\n";
            }
            if (tunerConnect()) {
                std::cout << "[SDR] connected; setting frequency to " << freqKHz << " kHz...\n";
                const bool okFreq = tunerSetFrequency(requestedFrequencyHz.load());
                const bool okRate = tunerSetSampleRate(INPUT_RATE);
                if (!okFreq || !okRate) {
                    std::cerr << "[SDR] warning: failed to initialize " << tunerName()
                              << " stream (setFrequency=" << (okFreq ? 1 : 0)
                              << ", setSampleRate=" << (okRate ? 1 : 0) << ")\n";
                    tunerDisconnect();
                    return;
                }
                if (verboseLogging) {
                    std::cout << "[SDR] applying TEF AGC mode " << requestedAGCMode.load()
                              << " and custom gain flags G" << requestedCustomGain.load() << "...\n";
                }
                rtlConnected = true;
                applyRtlGainAndAgc("connect/apply");
            } else {
                std::cerr << "[SDR] warning: failed to connect to " << tunerName() << "\n";
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
    StereoDecoder stereo(INPUT_RATE, OUTPUT_RATE);
    {
        std::string blendMode = config.processing.stereo_blend;
        std::transform(blendMode.begin(), blendMode.end(), blendMode.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        if (blendMode == "soft") {
            stereo.setBlendMode(StereoDecoder::BlendMode::Soft);
        } else if (blendMode == "aggressive") {
            stereo.setBlendMode(StereoDecoder::BlendMode::Aggressive);
        } else {
            stereo.setBlendMode(StereoDecoder::BlendMode::Normal);
        }
    }
    AFPostProcessor afPost(INPUT_RATE, OUTPUT_RATE);
    const std::size_t dspBlockSize = static_cast<std::size_t>(std::clamp(config.processing.dsp_block_samples, 1024, 32768));
    fm_tuner::dsp::Runtime dspRuntime(dspBlockSize, verboseLogging);
    if (verboseLogging) {
        std::cout << "[DSP] block_samples=" << dspBlockSize << "\n";
    }
    dspRuntime.addResetHandler([&]() {
        demod.reset();
        stereo.reset();
        afPost.reset();
    });
    int appliedBandwidthHz = requestedBandwidthHz.load();
    int appliedDeemphasis = requestedDeemphasis.load();
    bool appliedForceMono = requestedForceMono.load();
    bool appliedEffectiveForceMono = appliedForceMono;
    constexpr size_t kRetuneMuteSamples = static_cast<size_t>(OUTPUT_RATE / 25); // ~40 ms at 32 kHz
    float rfLevelFiltered = 0.0f;
    bool rfLevelInitialized = false;
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
    const std::string& audioDeviceToUse = !audioDevice.empty() ? audioDevice : config.audio.device;
    if (!audioOut.init(enableSpeaker, wavFile, audioDeviceToUse, verboseLogging)) {
        std::cerr << "[AUDIO] failed to initialize audio output\n";
        tunerDisconnect();
        return 1;
    }
    audioOut.setVolumePercent(requestedVolume.load());
    if (verboseLogging) {
        std::cout << "[AUDIO] audio output initialized" << std::endl;
    }

    FILE* iqHandle = nullptr;
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
    auto writeIqCapture = [&](const uint8_t* data, size_t sampleCount) {
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
    xdrServer.setForceMonoCallback([&](bool forceMono) {
        requestedForceMono = forceMono;
    });
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
            std::cerr << "[AUTO] warning: auto-start requested but " << tunerName() << " connect failed\n";
        }
    }

    std::atomic<bool> rdsStop(false);
    std::atomic<bool> rdsReset(false);
    std::mutex rdsQueueMutex;
    std::condition_variable rdsQueueCv;
    std::deque<std::vector<float>> rdsQueue;
    constexpr size_t RDS_QUEUE_LIMIT = 32;

    auto queueRdsBlock = [&](const float* samples, size_t count) {
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
            std::vector<float>& block = rdsQueue.back();
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
                rds.process(block.data(), block.size(), [&](const RDSGroup& group) {
                    xdrServer.updateRDS(group.blockA, group.blockB, group.blockC, group.blockD, group.errors);
                });
            }
        }
    });

    if (verboseLogging) {
        std::cout << "[APP] waiting for client connection on port " << xdrPort << "...\n";
        std::cout << "[APP] press Ctrl+C to stop.\n";
    }

    // Direct RTL-SDR async callback is configured to 16384 bytes (8192 IQ samples).
    // Using 8192-sample reads avoids deterministic "short read" logs at 256 ksps.
    const size_t BUF_SAMPLES = dspRuntime.blockSize();
    const auto noDataSleep = useDirectRtlSdr ? std::chrono::milliseconds(2)
                                             : std::chrono::milliseconds(10);
    const auto scanRetrySleep = useDirectRtlSdr ? std::chrono::milliseconds(2)
                                                : std::chrono::milliseconds(5);
    std::vector<uint8_t> iqBufferStorage(BUF_SAMPLES * 2, 0);
    std::vector<float> demodBufferStorage(BUF_SAMPLES, 0.0f);
    std::vector<float> stereoLeftStorage(BUF_SAMPLES, 0.0f);
    std::vector<float> stereoRightStorage(BUF_SAMPLES, 0.0f);
    std::vector<float> audioLeftStorage(BUF_SAMPLES, 0.0f);
    std::vector<float> audioRightStorage(BUF_SAMPLES, 0.0f);
    uint8_t* iqBuffer = iqBufferStorage.data();
    float* demodBuffer = demodBufferStorage.data();
    float* stereoLeft = stereoLeftStorage.data();
    float* stereoRight = stereoRightStorage.data();
    float* audioLeft = audioLeftStorage.data();
    float* audioRight = audioRightStorage.data();
    size_t retuneMuteSamplesRemaining = 0;
    size_t retuneMuteTotalSamples = 0;

    int consecutiveReadFailures = 0;
    bool scanActive = false;
    auto lastGainDown = std::chrono::steady_clock::now() - std::chrono::seconds(5);
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
                          << " bw=" << scanConfig.bandwidthHz
                          << " mode=" << (scanConfig.continuous ? "continuous" : "single")
                          << "\n";
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
            // Clear pilot/stereo state on retune to avoid carrying lock across stations.
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
            applyRtlGainAndAgc((gainChanged && agcChanged) ? "runtime/update(A+G)"
                                                           : (agcChanged ? "runtime/update(A)"
                                                                         : "runtime/update(G)"));
        }
        if (bandwidthChanged) {
            const int targetBandwidthHz = requestedBandwidthHz.load();
            if (targetBandwidthHz != appliedBandwidthHz) {
                demod.setBandwidthHz(targetBandwidthHz);
                if (verboseLogging) {
                    std::cout << "[BW] applied W" << targetBandwidthHz
                              << " (previous W" << appliedBandwidthHz << ")\n";
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
                        samples = tunerReadIQ(iqBuffer, BUF_SAMPLES);
                        if (samples == 0) {
                            std::this_thread::sleep_for(scanRetrySleep);
                        }
                    }
                    if (samples > 0) {
                        writeIqCapture(iqBuffer, samples);
                        const double powerSum = computeNormalizedIqPowerSum(iqBuffer, samples);
                        powerAccum += powerSum / static_cast<double>(samples * 2);
                        validReads++;
                    }
                }
                if (validReads == 0) {
                    continue;
                }
                const double avgPower = powerAccum / static_cast<double>(validReads);
                const double dbfs = 10.0 * std::log10(avgPower + 1e-12);
                const double gainCompDb = static_cast<double>(effectiveAppliedGainDb()) * kSignalGainCompFactor;
                const double compensatedDbfs = dbfs - gainCompDb;
                const double kRfFloorDbfs = config.sdr.signal_floor_dbfs;
                const double kRfCeilDbfs = std::max(config.sdr.signal_ceil_dbfs, kRfFloorDbfs + 1.0);
                const double norm = (compensatedDbfs - kRfFloorDbfs) / (kRfCeilDbfs - kRfFloorDbfs);
                const float rfLevel = std::clamp(static_cast<float>(norm * 90.0), 0.0f, 90.0f);

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

        size_t samples = tunerReadIQ(iqBuffer, BUF_SAMPLES);
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
        if (verboseLogging && samples < BUF_SAMPLES) {
            static uint32_t shortIqReadCount = 0;
            const uint32_t count = ++shortIqReadCount;
            if (count <= 5 || (count % 50) == 0) {
                std::cerr << "[SDR] short IQ read (" << count << "): "
                          << samples << "/" << BUF_SAMPLES << " samples\n";
            }
        }
        consecutiveReadFailures = 0;

        // RF-domain strength estimate from raw IQ power before demodulation.
        const double powerSum = computeNormalizedIqPowerSum(iqBuffer, samples);
        const size_t iqCount = samples * 2;
        const double avgPower = (iqCount > 0) ? (powerSum / static_cast<double>(iqCount)) : 0.0;
        const double dbfs = 10.0 * std::log10(avgPower + 1e-12);
        size_t hardClippedCount = 0;
        size_t nearClippedCount = 0;
        for (size_t i = 0; i < iqCount; i++) {
            const uint8_t v = iqBuffer[i];
            if (v <= 1 || v >= 254) {
                hardClippedCount++;
            }
            // Near-rail detector is more useful with RTL 8-bit samples than hard 0/255 hits only.
            if (v <= 8 || v >= 247) {
                nearClippedCount++;
            }
        }
        const double hardClipRatio = (iqCount > 0) ? (static_cast<double>(hardClippedCount) / static_cast<double>(iqCount)) : 0.0;
        const double nearClipRatio = (iqCount > 0) ? (static_cast<double>(nearClippedCount) / static_cast<double>(iqCount)) : 0.0;
        const double clipRatio = std::max(hardClipRatio, nearClipRatio);
        // Gain-normalize ADC-domain dBFS for a more stable RF-strength estimate.
        const double gainCompDb = static_cast<double>(effectiveAppliedGainDb()) * kSignalGainCompFactor;
        const double compensatedDbfs = dbfs - gainCompDb;
        const double kRfFloorDbfs = config.sdr.signal_floor_dbfs;
        const double kRfCeilDbfs = std::max(config.sdr.signal_ceil_dbfs, kRfFloorDbfs + 1.0);
        const double norm = (compensatedDbfs - kRfFloorDbfs) / (kRfCeilDbfs - kRfFloorDbfs);
        const float rfLevel = std::clamp(static_cast<float>(norm * 90.0), 0.0f, 90.0f);
        if (!rfLevelInitialized) {
            rfLevelFiltered = rfLevel;
            rfLevelInitialized = true;
        } else {
            rfLevelFiltered = rfLevelFiltered * 0.70f + rfLevel * 0.30f;
        }
        const bool effectiveForceMono = targetForceMono;
        if (effectiveForceMono != appliedEffectiveForceMono) {
            stereo.setForceMono(effectiveForceMono);
            appliedEffectiveForceMono = effectiveForceMono;
        }

        size_t outSamples = 0;
        bool stereoDetected = false;
        int pilotTenthsKHz = 0;
        if (!config.processing.stereo) {
            outSamples = demod.processSplit(iqBuffer, demodBuffer, audioLeft, samples);
            queueRdsBlock(demodBuffer, samples);
            for (size_t i = 0; i < outSamples; i++) {
                const float mono = audioLeft[i] * 0.5f;
                audioLeft[i] = mono;
                audioRight[i] = mono;
            }
        } else {
            // Feed RDS with full-rate discriminator MPX (includes 57 kHz RDS subcarrier).
            // redsea expects composite MPX, not post-audio-filtered baseband.
            demod.processSplit(iqBuffer, demodBuffer, nullptr, samples);
            queueRdsBlock(demodBuffer, samples);
            const size_t stereoSamples = stereo.processAudio(demodBuffer, stereoLeft, stereoRight, samples);
            outSamples = afPost.process(stereoLeft, stereoRight, stereoSamples, audioLeft, audioRight, BUF_SAMPLES);
            stereoDetected = stereo.isStereo();
            pilotTenthsKHz = stereo.getPilotLevelTenthsKHz();
        }
        const bool stereoIndicator = stereoDetected ||
                                     (effectiveForceMono && config.processing.stereo && pilotTenthsKHz >= 20);
        xdrServer.updateSignal(rfLevelFiltered, stereoIndicator, effectiveForceMono, -1, -1);
        xdrServer.updatePilot(pilotTenthsKHz);

        for (size_t i = 0; i < outSamples; i++) {
            audioLeft[i] = std::clamp(audioLeft[i], -1.0f, 1.0f);
            audioRight[i] = std::clamp(audioRight[i], -1.0f, 1.0f);
        }

        if (retuneMuteSamplesRemaining > 0 && outSamples > 0) {
            const size_t muteCount = std::min(outSamples, retuneMuteSamplesRemaining);
            const size_t alreadyMuted = (retuneMuteTotalSamples > retuneMuteSamplesRemaining)
                ? (retuneMuteTotalSamples - retuneMuteSamplesRemaining)
                : 0;
            const size_t fadeSamples = std::max<size_t>(
                1,
                std::min(static_cast<size_t>(OUTPUT_RATE / 200), retuneMuteTotalSamples / 2)); // up to ~5 ms
            for (size_t i = 0; i < muteCount; i++) {
                const size_t idx = alreadyMuted + i;
                float gain = 0.0f;
                if (idx < fadeSamples) {
                    gain = 1.0f - (static_cast<float>(idx) / static_cast<float>(fadeSamples));
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
