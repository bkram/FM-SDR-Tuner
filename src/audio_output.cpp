#include "audio_output.h"
#include <iostream>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <cctype>

#ifdef __APPLE__
namespace {
std::string trim(const std::string& value) {
    size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start])) != 0) {
        start++;
    }
    size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        end--;
    }
    return value.substr(start, end - start);
}

std::string normalizeSelector(const std::string& rawSelector) {
    std::string selector = trim(rawSelector);
    if (selector.size() >= 2) {
        const char first = selector.front();
        const char last = selector.back();
        if ((first == '"' && last == '"') || (first == '\'' && last == '\'')) {
            selector = trim(selector.substr(1, selector.size() - 2));
        }
    }
    return selector;
}

std::string toLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool parseDeviceIndex(const std::string& selector, int& outIndex) {
    if (selector.empty()) {
        return false;
    }
    for (char c : selector) {
        if (c < '0' || c > '9') {
            return false;
        }
    }
    outIndex = std::stoi(selector);
    return true;
}

PaDeviceIndex selectOutputDevice(const std::string& selector) {
    if (selector.empty()) {
        return Pa_GetDefaultOutputDevice();
    }

    int requestedIndex = -1;
    if (parseDeviceIndex(selector, requestedIndex)) {
        const int deviceCount = Pa_GetDeviceCount();
        if (requestedIndex >= 0 && requestedIndex < deviceCount) {
            const PaDeviceInfo* info = Pa_GetDeviceInfo(requestedIndex);
            if (info && info->maxOutputChannels > 0) {
                return static_cast<PaDeviceIndex>(requestedIndex);
            }
        }
        return paNoDevice;
    }

    const std::string needle = toLower(selector);
    const int deviceCount = Pa_GetDeviceCount();
    for (int i = 0; i < deviceCount; i++) {
        const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
        if (!info || info->maxOutputChannels <= 0 || !info->name) {
            continue;
        }
        if (toLower(info->name).find(needle) != std::string::npos) {
            return static_cast<PaDeviceIndex>(i);
        }
    }

    return paNoDevice;
}

void printOutputDeviceList() {
    const int deviceCount = Pa_GetDeviceCount();
    std::cerr << "Available PortAudio output devices:\n";
    for (int i = 0; i < deviceCount; i++) {
        const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
        if (!info || info->maxOutputChannels <= 0 || !info->name) {
            continue;
        }
        std::cerr << "  [" << i << "] " << info->name << "\n";
    }
}
}  // namespace
#endif

AudioOutput::AudioOutput()
    : m_enableSpeaker(false)
    , m_wavHandle(nullptr)
    , m_running(false)
    , m_wavDataSize(0)
    , m_writeIndex(0)
    , m_readIndex(0)
#ifdef __APPLE__
    , m_paStream(nullptr)
#endif
{
    m_circularBuffer.resize(65536 * 2);
}

AudioOutput::~AudioOutput() {
    shutdown();
}

bool AudioOutput::init(bool enableSpeaker, const std::string& wavFile, const std::string& deviceSelector, bool verboseLogging) {
    m_enableSpeaker = enableSpeaker;
    m_wavFile = wavFile;

    if (!wavFile.empty()) {
        if (!initWAV(wavFile)) {
            std::cerr << "Failed to initialize WAV file" << std::endl;
            return false;
        }
    }

#ifdef __APPLE__
    if (enableSpeaker) {
        const std::string normalizedSelector = normalizeSelector(deviceSelector);
        if (verboseLogging) {
            std::cerr << "[Audio] device selector raw='" << deviceSelector
                      << "' normalized='" << normalizedSelector << "'\n";
        }
        PaError err = Pa_Initialize();
        if (err != paNoError) {
            std::cerr << "PortAudio init failed: " << Pa_GetErrorText(err) << std::endl;
            return false;
        } else {
            PaStreamParameters outputParams;
            outputParams.device = selectOutputDevice(normalizedSelector);
            if (outputParams.device == paNoDevice) {
                std::cerr << "PortAudio device not found for selector: " << normalizedSelector << std::endl;
                printOutputDeviceList();
                Pa_Terminate();
                return false;
            }
            outputParams.channelCount = CHANNELS;
            outputParams.sampleFormat = paFloat32;
            const PaDeviceInfo* deviceInfo = Pa_GetDeviceInfo(outputParams.device);
            if (!deviceInfo) {
                std::cerr << "PortAudio failed to get device info for selected output device" << std::endl;
                Pa_Terminate();
                return false;
            }
            outputParams.suggestedLatency = deviceInfo->defaultLowOutputLatency;
            outputParams.hostApiSpecificStreamInfo = nullptr;
            if (verboseLogging) {
                if (normalizedSelector.empty()) {
                    std::cerr << "Using default output device [" << outputParams.device << "]: " << deviceInfo->name << std::endl;
                } else {
                    std::cerr << "Using selected output device [" << outputParams.device << "]: " << deviceInfo->name << std::endl;
                }
            }

            err = Pa_OpenStream(&m_paStream, nullptr, &outputParams, SAMPLE_RATE, FRAMES_PER_BUFFER, paClipOff, nullptr, nullptr);
            if (err != paNoError) {
                std::cerr << "PortAudio open failed: " << Pa_GetErrorText(err) << std::endl;
                Pa_Terminate();
                return false;
            } else {
                err = Pa_StartStream(m_paStream);
                if (err != paNoError) {
                    std::cerr << "PortAudio start failed: " << Pa_GetErrorText(err) << std::endl;
                    Pa_CloseStream(m_paStream);
                    m_paStream = nullptr;
                    Pa_Terminate();
                    return false;
                } else if (verboseLogging) {
                    std::cerr << "PortAudio started successfully" << std::endl;
                }
            }
        }
    }
#endif

    m_running = true;
    return true;
}

void AudioOutput::shutdown() {
    m_running = false;

#ifdef __APPLE__
    if (m_paStream) {
        Pa_StopStream(m_paStream);
        Pa_CloseStream(m_paStream);
        m_paStream = nullptr;
    }
    Pa_Terminate();
#endif

    closeWAV();
}

bool AudioOutput::initWAV(const std::string& filename) {
    m_wavHandle = fopen(filename.c_str(), "wb");
    if (!m_wavHandle) {
        return false;
    }

    m_wavDataSize = 0;
    writeWAVHeader();
    return true;
}

void AudioOutput::writeWAVHeader() {
    if (!m_wavHandle) return;

    uint32_t sampleRate = SAMPLE_RATE;
    uint16_t numChannels = CHANNELS;
    uint16_t bitsPerSample = BITS_PER_SAMPLE;
    uint32_t byteRate = sampleRate * numChannels * bitsPerSample / 8;
    uint16_t blockAlign = numChannels * bitsPerSample / 8;
    uint32_t dataSize = m_wavDataSize;

    fseek(m_wavHandle, 0, SEEK_SET);

    fwrite("RIFF", 1, 4, m_wavHandle);
    uint32_t fileSize = 36 + dataSize;
    fwrite(&fileSize, 4, 1, m_wavHandle);
    fwrite("WAVE", 1, 4, m_wavHandle);

    fwrite("fmt ", 1, 4, m_wavHandle);
    uint32_t fmtSize = 16;
    fwrite(&fmtSize, 4, 1, m_wavHandle);
    uint16_t audioFormat = 1;
    fwrite(&audioFormat, 2, 1, m_wavHandle);
    fwrite(&numChannels, 2, 1, m_wavHandle);
    fwrite(&sampleRate, 4, 1, m_wavHandle);
    fwrite(&byteRate, 4, 1, m_wavHandle);
    fwrite(&blockAlign, 2, 1, m_wavHandle);
    fwrite(&bitsPerSample, 2, 1, m_wavHandle);

    fwrite("data", 1, 4, m_wavHandle);
    fwrite(&dataSize, 4, 1, m_wavHandle);
}

bool AudioOutput::writeWAVData(const float* left, const float* right, size_t numSamples) {
    if (!m_wavHandle) return false;

    int16_t* buffer = new int16_t[numSamples * CHANNELS];

    for (size_t i = 0; i < numSamples; i++) {
        float l = std::max(-1.0f, std::min(1.0f, left[i]));
        float r = std::max(-1.0f, std::min(1.0f, right[i]));
        buffer[i * 2] = static_cast<int16_t>(l * 32767.0f);
        buffer[i * 2 + 1] = static_cast<int16_t>(r * 32767.0f);
    }

    size_t written = fwrite(buffer, sizeof(int16_t), numSamples * CHANNELS, m_wavHandle);
    m_wavDataSize += written * sizeof(int16_t);

    delete[] buffer;
    return written == numSamples * CHANNELS;
}

void AudioOutput::closeWAV() {
    if (m_wavHandle) {
        writeWAVHeader();
        fclose(m_wavHandle);
        m_wavHandle = nullptr;
    }
}

bool AudioOutput::write(const float* left, const float* right, size_t numSamples) {
    if (!m_running) return false;

    if (m_wavHandle) {
        writeWAVData(left, right, numSamples);
    }

#ifdef __APPLE__
    if (m_enableSpeaker && m_paStream) {
        float buffer[FRAMES_PER_BUFFER * CHANNELS];
        size_t toWrite = numSamples;
        size_t written = 0;
        
        while (toWrite > 0) {
            size_t chunk = std::min(toWrite, (size_t)FRAMES_PER_BUFFER);
            for (size_t i = 0; i < chunk; i++) {
                buffer[i * 2] = left[written + i];
                buffer[i * 2 + 1] = right[written + i];
            }
            Pa_WriteStream(m_paStream, buffer, chunk);
            written += chunk;
            toWrite -= chunk;
        }
    }
#endif

    return true;
}
