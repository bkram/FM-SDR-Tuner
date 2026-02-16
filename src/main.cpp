#include <iostream>
#include <csignal>
#include <atomic>
#include <cstring>
#include <getopt.h>

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

    std::cout << "Connecting to rtl_tcp at " << tcpHost << ":" << tcpPort << "...\n";

    RTLTCPClient rtlClient(tcpHost, tcpPort);
    if (!rtlClient.connect()) {
        std::cerr << "Warning: Failed to connect to rtl_tcp server, running in demo mode\n";
    } else {
        std::cout << "Connected. Setting frequency to " << freqKHz << " kHz...\n";
        rtlClient.setFrequency(freqKHz * 1000);
        rtlClient.setSampleRate(1024000);
        if (gain >= 0) {
            std::cout << "Setting gain to " << gain << " dB...\n";
            rtlClient.setGain(gain);
        }
    }

    constexpr int INPUT_RATE = 1024000;
    constexpr int OUTPUT_RATE = 32000;

    FMDemod demod(INPUT_RATE, OUTPUT_RATE);
    StereoDecoder stereo(OUTPUT_RATE);

    AudioOutput audioOut;
    std::cerr << "Initializing audio output..." << std::endl;
    if (!audioOut.init(enableSpeaker, wavFile)) {
        std::cerr << "Failed to initialize audio output\n";
        rtlClient.disconnect();
        return 1;
    }
    std::cerr << "Audio output initialized" << std::endl;

    XDRServer xdrServer;
    if (!xdrPassword.empty()) {
        xdrServer.setPassword(xdrPassword);
    }
    if (xdrGuestMode) {
        xdrServer.setGuestMode(true);
    }
    xdrServer.setFrequencyCallback([&](uint32_t freqHz) {
        std::cout << "Tuning to " << (freqHz / 1000) << " kHz\n";
        rtlClient.setFrequency(freqHz);
    });

    if (!xdrServer.start()) {
        std::cerr << "Failed to start XDR server\n";
    }

    std::cout << "Tuning to " << freqKHz << " kHz, streaming audio...\n";
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
        size_t samples = rtlClient.readIQ(iqBuffer, BUF_SAMPLES);
        if (samples == 0) {
            usleep(10000);
            continue;
        }

        demod.processNoDownsample(reinterpret_cast<int8_t*>(iqBuffer), monoBuffer, samples);

        stereo.process(monoBuffer, left, right, samples);

        size_t outSamples = 0;
        for (size_t i = 0; i + DECIMATE <= samples; i += DECIMATE) {
            float sumL = 0, sumR = 0;
            for (size_t j = 0; j < DECIMATE; j++) {
                sumL += left[i + j];
                sumR += right[i + j];
            }
            downsampledLeft[outSamples] = sumL / DECIMATE;
            downsampledRight[outSamples] = sumR / DECIMATE;
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
