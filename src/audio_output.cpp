#include "audio_output.h"
#include <cmath>
#include <cstring>
#include <iostream>
#include <algorithm>

AudioOutput::AudioOutput()
    : m_enableSpeaker(false)
    , m_wavHandle(nullptr)
    , m_running(false)
    , m_wavDataSize(0)
    , m_writeIndex(0)
    , m_readIndex(0)
#ifdef __APPLE__
    , m_audioQueue(nullptr)
#endif
{
    m_circularBuffer.resize(CIRCULAR_BUFFER_FRAMES * CHANNELS, 0.0f);
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
        if (!initAudioQueue()) {
            std::cerr << "Warning: Failed to initialize audio queue" << std::endl;
        }
    }
#endif

    m_running = true;
    return true;
}

void AudioOutput::shutdown() {
    m_running = false;

#ifdef __APPLE__
    stopAudioQueue();
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
    if (m_enableSpeaker && m_audioQueue) {
        for (size_t i = 0; i < numSamples; i++) {
            int writeIdx = m_writeIndex.fetch_add(1);
            int bufIdx = writeIdx % CIRCULAR_BUFFER_FRAMES;
            if (writeIdx >= CIRCULAR_BUFFER_FRAMES) {
                m_writeIndex.fetch_sub(CIRCULAR_BUFFER_FRAMES);
            }
            m_circularBuffer[bufIdx * CHANNELS] = left[i];
            m_circularBuffer[bufIdx * CHANNELS + 1] = right[i];
        }
    }
#endif

    return true;
}

#ifdef __APPLE__

void AudioOutput::audioQueueCallback(void* inUserData, AudioQueueRef inAQ, AudioQueueBufferRef inBuffer) {
    AudioOutput* audio = (AudioOutput*)inUserData;
    
    float* outSamples = (float*)inBuffer->mAudioData;
    
    int rawWriteIdx = audio->m_writeIndex.load();
    int writeIdx;
    if (rawWriteIdx >= CIRCULAR_BUFFER_FRAMES) {
        writeIdx = rawWriteIdx % CIRCULAR_BUFFER_FRAMES;
    } else {
        writeIdx = rawWriteIdx;
    }
    int readIdx = audio->m_readIndex.load();
    
    int available = (writeIdx >= readIdx) ? 
        (writeIdx - readIdx) : 
        (CIRCULAR_BUFFER_FRAMES - readIdx + writeIdx);
    
    int framesToRead = std::min(QUEUE_BUFFER_FRAMES, available);
    
    for (int i = 0; i < framesToRead * CHANNELS; i++) {
        outSamples[i] = audio->m_circularBuffer[readIdx * CHANNELS + i];
    }
    
    for (int i = framesToRead * CHANNELS; i < QUEUE_BUFFER_FRAMES * CHANNELS; i++) {
        outSamples[i] = 0.0f;
    }
    
    readIdx += framesToRead;
    if (readIdx >= CIRCULAR_BUFFER_FRAMES) readIdx -= CIRCULAR_BUFFER_FRAMES;
    audio->m_readIndex.store(readIdx);
    
    inBuffer->mAudioDataByteSize = QUEUE_BUFFER_FRAMES * CHANNELS * sizeof(float);
    AudioQueueEnqueueBuffer(audio->m_audioQueue, inBuffer, 0, nullptr);
}

bool AudioOutput::initAudioQueue() {
    AudioStreamBasicDescription format = {0};
    format.mSampleRate = SAMPLE_RATE;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    format.mBytesPerPacket = CHANNELS * sizeof(float);
    format.mFramesPerPacket = 1;
    format.mBytesPerFrame = CHANNELS * sizeof(float);
    format.mChannelsPerFrame = CHANNELS;
    format.mBitsPerChannel = 32;

    OSStatus status = AudioQueueNewOutput(&format, audioQueueCallback, this, nullptr, nullptr, 0, &m_audioQueue);
    if (status != noErr) {
        std::cerr << "AudioQueueNewOutput failed: " << status << std::endl;
        return false;
    }

    UInt32 bufferSize = QUEUE_BUFFER_FRAMES * CHANNELS * sizeof(float);
    for (int i = 0; i < NUM_QUEUE_BUFFERS; i++) {
        status = AudioQueueAllocateBuffer(m_audioQueue, bufferSize, &m_buffers[i]);
        if (status != noErr) {
            std::cerr << "AudioQueueAllocateBuffer failed: " << status << std::endl;
            return false;
        }
        audioQueueCallback(this, m_audioQueue, m_buffers[i]);
    }

    status = AudioQueueStart(m_audioQueue, nullptr);
    if (status != noErr) {
        std::cerr << "AudioQueueStart failed: " << status << std::endl;
        return false;
    }
    
    std::cerr << "AudioQueue started successfully" << std::endl;
    return true;
}

void AudioOutput::stopAudioQueue() {
    if (m_audioQueue) {
        AudioQueueStop(m_audioQueue, true);
        AudioQueueDispose(m_audioQueue, true);
        m_audioQueue = nullptr;
    }
}

#endif
