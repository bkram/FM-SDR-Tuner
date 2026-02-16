#include <iostream>
#include <csignal>
#include <atomic>
#include <cstring>
#include <getopt.h>
#include <algorithm>
#include <cmath>

#include "rtl_tcp_client.h"
#include "fm_demod.h"
#include "stereo_decoder.h"
#include "xdr_server.h"
#include "audio_output.h"

static std::atomic<bool> g_running(true);

void signalHandler(int) {
    g_running = false;
}

void printUsage(const char* prog) {
    std::cout << "Usage: " << prog << " [options]\n"
              << "Options:\n"
              << "  -t, --tcp <host:port>  rtl_tcp server address (default: localhost:1234)\n"
              << "  -f, --freq <khz>      Frequency in kHz (default: 88600)\n"
              << "  -g, --gain <db>       RTL-SDR gain in dB (default: auto)\n"
              << "  -w, --wav <file>      Output WAV file\n"
              << "  -s, --speaker          Enable speaker output\n"
              << "  -P, --password <pwd>   XDR server password\n"
              << "  -G, --guest            Enable guest mode (no password required)\n"
              << "  -h, --help             Show this help\n";
}

int main(int argc, char* argv[]) {
    std::string tcpHost = "localhost";
    uint16_t tcpPort = 1234;
    uint32_t freqKHz = 88600;
    int gain = -1;
    std::string wavFile;
    bool enableSpeaker = false;
    std::string xdrPassword;
    bool xdrGuestMode = false;

    static struct option longOptions[] = {
        {"tcp", required_argument, 0, 't'},
        {"freq", required_argument, 0, 'f'},
        {"gain", required_argument, 0, 'g'},
        {"wav", required_argument, 0, 'w'},
        {"speaker", no_argument, 0, 's'},
        {"password", required_argument, 0, 'P'},
        {"guest", no_argument, 0, 'G'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "t:f:g:w:sP:Gh", longOptions, nullptr)) != -1) {
        switch (opt) {
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
        std::cerr << "Error: must specify -w (wav file) or -s (speaker)\n";
        printUsage(argv[0]);
        return 1;
    }

    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    RTLTCPClient rtlClient(tcpHost, tcpPort);
    bool rtlConnected = false;
    std::atomic<uint32_t> requestedFrequencyHz(freqKHz * 1000);
    std::atomic<int> requestedGainDb(std::max(0, gain));
    std::atomic<int> requestedAGCMode(2);
    std::atomic<int> requestedVolume(100);
    std::atomic<int> requestedDeemphasis(0);
    std::atomic<bool> requestedForceMono(false);
    std::atomic<bool> pendingFrequency(false);
    std::atomic<bool> pendingGain(false);
    std::atomic<bool> pendingAGC(false);

    auto applyRtlGainAndAgc = [&]() {
        if (!rtlConnected) {
            return;
        }

        const bool manualGain = (requestedAGCMode.load() == 0);
        rtlClient.setGainMode(manualGain);
        rtlClient.setAGC(!manualGain);

        // rtl_tcp gain command uses 0.1 dB steps.
        const uint32_t gainTenthsDb = static_cast<uint32_t>(std::max(0, requestedGainDb.load()) * 10);
        rtlClient.setGain(gainTenthsDb);
    };

    auto connectTuner = [&]() {
        if (!rtlConnected) {
            std::cout << "Connecting to rtl_tcp at " << tcpHost << ":" << tcpPort << "...\n";
            if (rtlClient.connect()) {
                std::cout << "Connected. Setting frequency to " << freqKHz << " kHz...\n";
                rtlClient.setFrequency(requestedFrequencyHz.load());
                rtlClient.setSampleRate(1024000);
                std::cout << "Applying AGC mode " << requestedAGCMode.load()
                          << " and gain " << requestedGainDb.load() << " dB...\n";
                rtlConnected = true;
                applyRtlGainAndAgc();
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

    constexpr int INPUT_RATE = 1024000;
    constexpr int OUTPUT_RATE = 32000;

    FMDemod demod(INPUT_RATE, OUTPUT_RATE);
    StereoDecoder stereo(OUTPUT_RATE);
    int appliedDeemphasis = requestedDeemphasis.load();
    bool appliedForceMono = requestedForceMono.load();
    float rfLevelFiltered = 0.0f;
    bool rfLevelInitialized = false;
    demod.setDeemphasis(appliedDeemphasis == 1 ? 50 : 75);
    stereo.setForceMono(appliedForceMono);

    AudioOutput audioOut;
    std::cerr << "Initializing audio output..." << std::endl;
    if (!audioOut.init(enableSpeaker, wavFile)) {
        std::cerr << "Failed to initialize audio output\n";
        rtlClient.disconnect();
        return 1;
    }
    std::cerr << "Audio output initialized" << std::endl;

    std::atomic<bool> tunerActive(false);

    XDRServer xdrServer;
    if (!xdrPassword.empty()) {
        xdrServer.setPassword(xdrPassword);
    }
    if (xdrGuestMode) {
        xdrServer.setGuestMode(true);
    }
    xdrServer.setFrequencyCallback([&](uint32_t freqHz) {
        std::cout << "Tuning to " << (freqHz / 1000) << " kHz\n";
        requestedFrequencyHz = freqHz;
        pendingFrequency = true;
    });
    xdrServer.setVolumeCallback([&](int volume) {
        requestedVolume = std::clamp(volume, 0, 100);
    });
    xdrServer.setGainCallback([&](int newGain) {
        requestedGainDb = std::clamp(newGain, 0, 99);
        pendingGain = true;
    });
    xdrServer.setAGCCallback([&](int agcMode) {
        requestedAGCMode = std::clamp(agcMode, 0, 2);
        pendingAGC = true;
    });
    xdrServer.setModeCallback([&](int mode) {
        if (mode != 0) {
            std::cout << "Mode " << mode << " requested (FM demod path only)\n";
        }
    });
    xdrServer.setDeemphasisCallback([&](int deemphasis) {
        requestedDeemphasis = std::clamp(deemphasis, 0, 1);
    });
    xdrServer.setForceMonoCallback([&](bool forceMono) {
        requestedForceMono = forceMono;
    });
    xdrServer.setStartCallback([&]() {
        std::cout << "Tuner started by client\n";
        connectTuner();
        tunerActive = true;
    });
    xdrServer.setStopCallback([&]() {
        std::cout << "Tuner stopped by client\n";
        tunerActive = false;
        disconnectTuner();
    });

    if (!xdrServer.start()) {
        std::cerr << "Failed to start XDR server\n";
    }

    std::cout << "Waiting for client connection on port 7373...\n";
    std::cout << "Press Ctrl+C to stop.\n";

    const size_t BUF_SAMPLES = 8192;
    const size_t DECIMATE = 32;
    uint8_t* iqBuffer = new uint8_t[BUF_SAMPLES * 2];
    float* monoBuffer = new float[BUF_SAMPLES];
    float* left = new float[BUF_SAMPLES];
    float* right = new float[BUF_SAMPLES];
    float* downsampledLeft = new float[BUF_SAMPLES / DECIMATE];
    float* downsampledRight = new float[BUF_SAMPLES / DECIMATE];

    while (g_running) {
        if (!tunerActive) {
            usleep(10000);
            continue;
        }

        if (rtlConnected && pendingFrequency.exchange(false)) {
            rtlClient.setFrequency(requestedFrequencyHz.load());
        }
        const bool gainChanged = pendingGain.exchange(false);
        const bool agcChanged = pendingAGC.exchange(false);
        if (rtlConnected && (gainChanged || agcChanged)) {
            applyRtlGainAndAgc();
        }

        int targetDeemphasis = requestedDeemphasis.load();
        if (targetDeemphasis != appliedDeemphasis) {
            demod.setDeemphasis(targetDeemphasis == 1 ? 50 : 75);
            appliedDeemphasis = targetDeemphasis;
        }

        bool targetForceMono = requestedForceMono.load();
        if (targetForceMono != appliedForceMono) {
            stereo.setForceMono(targetForceMono);
            appliedForceMono = targetForceMono;
        }

        size_t samples = rtlClient.readIQ(iqBuffer, BUF_SAMPLES);
        if (samples == 0) {
            usleep(10000);
            continue;
        }

        // RF-domain strength estimate from raw IQ power before demodulation.
        double powerSum = 0.0;
        for (size_t i = 0; i < samples; i++) {
            const double iNorm = (static_cast<int>(iqBuffer[i * 2]) - 127.5) / 127.5;
            const double qNorm = (static_cast<int>(iqBuffer[i * 2 + 1]) - 127.5) / 127.5;
            powerSum += iNorm * iNorm + qNorm * qNorm;
        }
        const double avgPower = (samples > 0) ? (powerSum / static_cast<double>(samples)) : 0.0;
        const double dbfs = 10.0 * std::log10(avgPower + 1e-12);
        const float rfLevel = std::clamp(static_cast<float>(dbfs + 90.0), 0.0f, 90.0f);
        if (!rfLevelInitialized) {
            rfLevelFiltered = rfLevel;
            rfLevelInitialized = true;
        } else {
            rfLevelFiltered = rfLevelFiltered * 0.85f + rfLevel * 0.15f;
        }

        demod.processNoDownsample(reinterpret_cast<int8_t*>(iqBuffer), monoBuffer, samples);
        stereo.process(monoBuffer, left, right, samples);
        xdrServer.updateSignal(rfLevelFiltered, stereo.isStereo(), requestedForceMono.load(), -1, -1);

        size_t outSamples = 0;
        for (size_t i = 0; i + DECIMATE <= samples; i += DECIMATE) {
            float sumL = 0, sumR = 0;
            for (size_t j = 0; j < DECIMATE; j++) {
                sumL += left[i + j];
                sumR += right[i + j];
            }
            float volumeScale = requestedVolume.load() / 100.0f;
            downsampledLeft[outSamples] = (sumL / DECIMATE) * volumeScale;
            downsampledRight[outSamples] = (sumR / DECIMATE) * volumeScale;
            outSamples++;
        }

        audioOut.write(downsampledLeft, downsampledRight, outSamples);
    }

    delete[] iqBuffer;
    delete[] monoBuffer;
    delete[] left;
    delete[] right;
    delete[] downsampledLeft;
    delete[] downsampledRight;

    audioOut.shutdown();
    xdrServer.stop();
    rtlClient.disconnect();

    std::cout << "Shutdown complete.\n";
    return 0;
}
