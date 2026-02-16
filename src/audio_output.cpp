#include "audio_output.h"
#include <iostream>
#include <cmath>
#include <cstring>

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

bool AudioOutput::init(bool enableSpeaker, const std::string& wavFile) {
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
        PaError err = Pa_Initialize();
        if (err != paNoError) {
            std::cerr << "PortAudio init failed: " << Pa_GetErrorText(err) << std::endl;
        } else {
            PaStreamParameters outputParams;
            outputParams.device = Pa_GetDefaultOutputDevice();
            outputParams.channelCount = CHANNELS;
            outputParams.sampleFormat = paFloat32;
            outputParams.suggestedLatency = Pa_GetDeviceInfo(outputParams.device)->defaultLowOutputLatency;
            outputParams.hostApiSpecificStreamInfo = nullptr;

            err = Pa_OpenStream(&m_paStream, nullptr, &outputParams, SAMPLE_RATE, FRAMES_PER_BUFFER, paClipOff, nullptr, nullptr);
            if (err != paNoError) {
                std::cerr << "PortAudio open failed: " << Pa_GetErrorText(err) << std::endl;
            } else {
                err = Pa_StartStream(m_paStream);
                if (err != paNoError) {
                    std::cerr << "PortAudio start failed: " << Pa_GetErrorText(err) << std::endl;
                } else {
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
