#include <iostream>
#include <csignal>
#include <atomic>
#include <cstring>
#include <getopt.h>
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

#include "rtl_tcp_client.h"
#include "fm_demod.h"
#include "stereo_decoder.h"
#include "af_post_processor.h"
#include "cpu_features.h"
#include "rds_decoder.h"
#include "xdr_server.h"
#include "audio_output.h"
#include "config.h"

static std::atomic<bool> g_running(true);

namespace {
double computeNormalizedIqPowerSum(const uint8_t* iq, size_t samples) {
    static const std::array<float, 256> kNormSqLut = []() {
        std::array<float, 256> lut{};
        for (int v = 0; v < 256; v++) {
            const float norm = (static_cast<float>(v) - 127.5f) / 127.5f;
            lut[static_cast<size_t>(v)] = norm * norm;
        }
        return lut;
    }();

    const size_t count = samples * 2;
    double powerSum = 0.0;
    for (size_t i = 0; i < count; i++) {
        powerSum += kNormSqLut[iq[i]];
    }
    return powerSum;
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
              << "  -f, --freq <khz>      Frequency in kHz (default: 88600)\n"
              << "  -g, --gain <db>       RTL-SDR gain in dB (default: auto)\n"
              << "  -w, --wav <file>      Output WAV file\n"
              << "  -s, --audio           Enable audio output\n"
              << "  -P, --password <pwd>   XDR server password\n"
              << "  -G, --guest            Enable guest mode (no password required)\n"
              << "  -h, --help             Show this help\n";
}

int main(int argc, char* argv[]) {
    constexpr int INPUT_RATE = 256000;
    constexpr int OUTPUT_RATE = 32000;

    std::cout << "FM-Tuner-SDR version " << FM_TUNER_SDR_VERSION << "\n"
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
    }
    if (config.audio.output_rate != OUTPUT_RATE ||
        config.audio.buffer_size != AudioOutput::FRAMES_PER_BUFFER) {
        std::cerr << "Warning: audio.output_rate/audio.buffer_size are fixed at "
                  << OUTPUT_RATE << "/" << AudioOutput::FRAMES_PER_BUFFER
                  << " in current SDR architecture; INI values are ignored\n";
    }

    std::string tcpHost = config.rtl_tcp.host;
    uint16_t tcpPort = config.rtl_tcp.port;
    uint32_t freqKHz = config.tuner.default_freq;
    int gain = config.tuner.default_gain;
    bool allowClientGainOverride = config.processing.allow_client_gain_override;
    std::string wavFile;
    bool enableSpeaker = false;
    std::string xdrPassword = config.xdr.password;
    bool xdrGuestMode = config.xdr.guest_mode;
    uint16_t xdrPort = config.xdr.port;
    bool autoReconnect = config.reconnection.auto_reconnect;

    static struct option longOptions[] = {
        {"config", required_argument, 0, 'c'},
        {"tcp", required_argument, 0, 't'},
        {"freq", required_argument, 0, 'f'},
        {"gain", required_argument, 0, 'g'},
        {"wav", required_argument, 0, 'w'},
        {"audio", no_argument, 0, 's'},
        {"password", required_argument, 0, 'P'},
        {"guest", no_argument, 0, 'G'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "c:t:f:g:w:sP:Gh", longOptions, nullptr)) != -1) {
        switch (opt) {
            case 'c':
                configPath = optarg;
                break;
            case 't': {
                std::string arg = optarg;
                size_t colon = arg.find(':');
                if (colon != std::string::npos) {
                    tcpHost = arg.substr(0, colon);
                    tcpPort = static_cast<uint16_t>(std::stoi(arg.substr(colon + 1)));
                } else {
                    tcpHost = arg;
                }
                break;
            }
            case 'f':
                freqKHz = std::stoi(optarg);
                break;
            case 'g':
                gain = std::stoi(optarg);
                break;
            case 'w':
                wavFile = optarg;
                break;
            case 's':
                enableSpeaker = true;
                break;
            case 'P':
                xdrPassword = optarg;
                break;
            case 'G':
                xdrGuestMode = true;
                break;
            case 'h':
                printUsage(argv[0]);
                return 0;
            default:
                printUsage(argv[0]);
                return 1;
        }
    }

    if (wavFile.empty() && !enableSpeaker) {
        std::cerr << "Error: must specify -w (wav file) or -s (audio)\n";
        printUsage(argv[0]);
        return 1;
    }

    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    RTLTCPClient rtlClient(tcpHost, tcpPort);
    bool rtlConnected = false;
    std::atomic<uint32_t> requestedFrequencyHz(freqKHz * 1000);
    std::atomic<int> requestedCustomGain(0);
    std::atomic<int> requestedAGCMode(std::clamp(config.processing.agc_mode, 0, 3));
    std::atomic<int> requestedBandwidthHz(0);
    std::atomic<int> requestedVolume(100);
    std::atomic<int> requestedDeemphasis(std::clamp(config.tuner.deemphasis, 0, 2));
    std::atomic<bool> requestedForceMono(false);
    std::atomic<bool> pendingFrequency(false);
    std::atomic<bool> pendingGain(false);
    std::atomic<bool> pendingAGC(false);
    std::atomic<bool> pendingBandwidth(false);

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

    auto applyRtlGainAndAgc = [&](const char* reason) {
        if (!rtlConnected) {
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
            // IMS bit maps to RTL automatic gain behavior.
            okGainMode = rtlClient.setGainMode(false);
            okAgc = rtlClient.setAGC(true);
        } else {
            okGainMode = rtlClient.setGainMode(true);
            okAgc = rtlClient.setAGC(false);
            okGain = rtlClient.setGain(static_cast<uint32_t>(gainDb * 10));
        }

        if (verboseLogging) {
            std::cout << "[RTL] " << reason
                      << " A" << agcMode
                      << " G" << formatCustomGain(custom)
                      << " (rf=" << rf << ",if=" << ifv << ")"
                      << " -> mode=" << (imsAgc ? "auto" : "manual")
                      << " tuner_gain=" << gainDb << " dB"
                      << " manual=" << (imsAgc ? 0 : 1)
                      << " rtl_agc=" << (imsAgc ? 1 : 0) << "\n";
        }
        if (!okGainMode || !okAgc || !okGain) {
            std::cerr << "[RTL] warning: gain/apply command failed"
                      << " setGainMode=" << (okGainMode ? 1 : 0)
                      << " setAGC=" << (okAgc ? 1 : 0)
                      << " setGain=" << (okGain ? 1 : 0) << "\n";
        }
    };

    auto connectTuner = [&]() {
        if (!rtlConnected) {
            std::cout << "Connecting to rtl_tcp at " << tcpHost << ":" << tcpPort << "...\n";
            if (rtlClient.connect()) {
                std::cout << "Connected. Setting frequency to " << freqKHz << " kHz...\n";
                rtlClient.setFrequency(requestedFrequencyHz.load());
                rtlClient.setSampleRate(INPUT_RATE);
                if (verboseLogging) {
                    std::cout << "Applying TEF AGC mode " << requestedAGCMode.load()
                              << " and custom gain flags G" << requestedCustomGain.load() << "...\n";
                }
                rtlConnected = true;
                applyRtlGainAndAgc("connect/apply");
            } else {
                std::cerr << "Warning: Failed to connect to rtl_tcp server\n";
            }
        }
    };

    auto disconnectTuner = [&]() {
        if (rtlConnected) {
            rtlClient.disconnect();
            rtlConnected = false;
            std::cout << "Disconnected from rtl_tcp\n";
        }
    };

    FMDemod demod(INPUT_RATE, OUTPUT_RATE);
    StereoDecoder stereo(INPUT_RATE, OUTPUT_RATE);
    AFPostProcessor afPost(INPUT_RATE, OUTPUT_RATE);
    int appliedBandwidthHz = requestedBandwidthHz.load();
    int appliedDeemphasis = requestedDeemphasis.load();
    bool appliedForceMono = requestedForceMono.load();
    bool appliedEffectiveForceMono = appliedForceMono;
    float rfLevelFiltered = 0.0f;
    bool rfLevelInitialized = false;
    if (appliedDeemphasis == 0) {
        afPost.setDeemphasis(50);
    } else if (appliedDeemphasis == 1) {
        afPost.setDeemphasis(75);
    } else {
        afPost.setDeemphasis(0);
    }
    stereo.setForceMono(appliedEffectiveForceMono);
    demod.setBandwidthHz(appliedBandwidthHz);

    AudioOutput audioOut;
    if (verboseLogging) {
        std::cerr << "Initializing audio output..." << std::endl;
    }
    if (!audioOut.init(enableSpeaker, wavFile, config.audio.device, verboseLogging)) {
        std::cerr << "Failed to initialize audio output\n";
        rtlClient.disconnect();
        return 1;
    }
    if (verboseLogging) {
        std::cerr << "Audio output initialized" << std::endl;
    }

    std::atomic<bool> tunerActive(false);

    XDRServer xdrServer(xdrPort);
    xdrServer.setVerboseLogging(verboseLogging);
    if (!xdrPassword.empty()) {
        xdrServer.setPassword(xdrPassword);
    }
    if (xdrGuestMode) {
        xdrServer.setGuestMode(true);
    }
    xdrServer.setFrequencyCallback([&](uint32_t freqHz) {
        if (verboseLogging) {
            std::cout << "Tuning to " << (freqHz / 1000) << " kHz\n";
        }
        requestedFrequencyHz = freqHz;
        pendingFrequency = true;
    });
    xdrServer.setVolumeCallback([&](int volume) {
        requestedVolume = std::clamp(volume, 0, 100);
    });
    xdrServer.setGainCallback([&](int newGain) -> bool {
        if (!allowClientGainOverride) {
            if (verboseLogging) {
                std::cout << "[XDR] G command ignored (allow_client_gain_override=false)\n";
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
        if (!allowClientGainOverride) {
            if (verboseLogging) {
                std::cout << "[XDR] A command ignored (allow_client_gain_override=false)\n";
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
            std::cout << "Mode " << mode << " requested (FM demod path only)\n";
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
            std::cout << "Tuner started by client\n";
        }
        connectTuner();
        tunerActive = true;
    });
    xdrServer.setStopCallback([&]() {
        if (verboseLogging) {
            std::cout << "Tuner stopped by client\n";
        }
        tunerActive = false;
        disconnectTuner();
    });

    if (!xdrServer.start()) {
        std::cerr << "Failed to start XDR server\n";
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

        std::vector<float> block(count);
        std::memcpy(block.data(), samples, count * sizeof(float));

        {
            std::lock_guard<std::mutex> lock(rdsQueueMutex);
            if (rdsQueue.size() >= RDS_QUEUE_LIMIT) {
                // Keep continuity for decoder lock; drop newest block under overload.
                return;
            }
            rdsQueue.emplace_back(std::move(block));
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
        std::cout << "Waiting for client connection on port " << xdrPort << "...\n";
        std::cout << "Press Ctrl+C to stop.\n";
    }

    const size_t BUF_SAMPLES = 8192;
    uint8_t* iqBuffer = new uint8_t[BUF_SAMPLES * 2];
    float* demodBuffer = new float[BUF_SAMPLES];
    float* stereoLeft = new float[BUF_SAMPLES];
    float* stereoRight = new float[BUF_SAMPLES];
    float* audioLeft = new float[BUF_SAMPLES];
    float* audioRight = new float[BUF_SAMPLES];

    int consecutiveReadFailures = 0;
    bool scanActive = false;
    XDRServer::ScanConfig scanConfig;
    uint32_t scanRestoreFreqHz = requestedFrequencyHz.load();
    int scanRestoreBandwidthHz = appliedBandwidthHz;
    auto restoreAfterScan = [&]() {
        requestedBandwidthHz = scanRestoreBandwidthHz;
        pendingBandwidth = true;
        rtlClient.setFrequency(scanRestoreFreqHz);
        requestedFrequencyHz = scanRestoreFreqHz;
        stereo.reset();
        afPost.reset();
        rdsReset = true;
        {
            std::lock_guard<std::mutex> lock(rdsQueueMutex);
            rdsQueue.clear();
        }
        rdsQueueCv.notify_one();
    };
    while (g_running) {
        if (!tunerActive) {
            usleep(10000);
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
            rtlClient.setFrequency(requestedFrequencyHz.load());
            // Clear pilot/stereo state on retune to avoid carrying lock across stations.
            stereo.reset();
            afPost.reset();
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

                rtlClient.setFrequency(static_cast<uint32_t>(f) * 1000U);

                size_t samples = 0;
                for (int retries = 0; retries < 4 && samples == 0; retries++) {
                    samples = rtlClient.readIQ(iqBuffer, BUF_SAMPLES);
                    if (samples == 0) {
                        usleep(5000);
                    }
                }
                if (samples == 0) {
                    continue;
                }

                const double powerSum = computeNormalizedIqPowerSum(iqBuffer, samples);
                const double avgPower = powerSum / static_cast<double>(samples);
                const double dbfs = 10.0 * std::log10(avgPower + 1e-12);
                const double gainCompDb = isImsAgcEnabled() ? 0.0 : static_cast<double>(calculateAppliedGainDb());
                const double compensatedDbfs = dbfs - gainCompDb;
                const float rfLevel = std::clamp(static_cast<float>((compensatedDbfs + 72.0) * 1.8), 0.0f, 90.0f);

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
            } else if (targetDeemphasis == 1) {
                afPost.setDeemphasis(75);
            } else {
                afPost.setDeemphasis(0);
            }
            appliedDeemphasis = targetDeemphasis;
        }

        bool targetForceMono = requestedForceMono.load();
        if (targetForceMono != appliedForceMono) {
            appliedForceMono = targetForceMono;
        }

        size_t samples = rtlClient.readIQ(iqBuffer, BUF_SAMPLES);
        if (samples == 0) {
            consecutiveReadFailures++;
            if (autoReconnect && rtlConnected && consecutiveReadFailures >= 20) {
                std::cerr << "[RTL] no IQ data, reconnecting...\n";
                disconnectTuner();
                connectTuner();
                consecutiveReadFailures = 0;
            }
            usleep(10000);
            continue;
        }
        consecutiveReadFailures = 0;

        // RF-domain strength estimate from raw IQ power before demodulation.
        const double powerSum = computeNormalizedIqPowerSum(iqBuffer, samples);
        const double avgPower = (samples > 0) ? (powerSum / static_cast<double>(samples)) : 0.0;
        const double dbfs = 10.0 * std::log10(avgPower + 1e-12);
        // Compensate by configured RTL gain so RF level is less "always high".
        const double gainCompDb = isImsAgcEnabled() ? 0.0 : static_cast<double>(calculateAppliedGainDb());
        const double compensatedDbfs = dbfs - gainCompDb;
        const float rfLevel = std::clamp(static_cast<float>((compensatedDbfs + 72.0) * 1.8), 0.0f, 90.0f);
        if (!rfLevelInitialized) {
            rfLevelFiltered = rfLevel;
            rfLevelInitialized = true;
        } else {
            rfLevelFiltered = rfLevelFiltered * 0.85f + rfLevel * 0.15f;
        }
        const bool effectiveForceMono = targetForceMono;
        if (effectiveForceMono != appliedEffectiveForceMono) {
            stereo.setForceMono(effectiveForceMono);
            appliedEffectiveForceMono = effectiveForceMono;
        }

        demod.processNoDownsample(iqBuffer, demodBuffer, samples);
        queueRdsBlock(demodBuffer, samples);
        const size_t stereoSamples = stereo.processAudio(demodBuffer, stereoLeft, stereoRight, samples);
        const size_t outSamples = afPost.process(stereoLeft, stereoRight, stereoSamples, audioLeft, audioRight, BUF_SAMPLES);
        xdrServer.updateSignal(rfLevelFiltered, stereo.isStereo(), effectiveForceMono, -1, -1);
        xdrServer.updatePilot(stereo.getPilotLevelTenthsKHz());

        // Keep small headroom to reduce hard clipping distortion at high modulation.
        const float volumeScale = (requestedVolume.load() / 100.0f) * 0.85f;
        for (size_t i = 0; i < outSamples; i++) {
            audioLeft[i] = std::clamp(audioLeft[i] * volumeScale, -1.0f, 1.0f);
            audioRight[i] = std::clamp(audioRight[i] * volumeScale, -1.0f, 1.0f);
        }

        if (outSamples > 0) {
            audioOut.write(audioLeft, audioRight, outSamples);
        }
    }

    delete[] iqBuffer;
    delete[] demodBuffer;
    delete[] stereoLeft;
    delete[] stereoRight;
    delete[] audioLeft;
    delete[] audioRight;

    rdsStop = true;
    rdsQueueCv.notify_all();
    if (rdsThread.joinable()) {
        rdsThread.join();
    }

    audioOut.shutdown();
    xdrServer.stop();
    rtlClient.disconnect();

    std::cout << "Shutdown complete.\n";
    return 0;
}
