#ifndef RTL_SDR_DEVICE_H
#define RTL_SDR_DEVICE_H

#include <cstddef>
#include <cstdint>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

class RTLSDRDevice {
public:
    explicit RTLSDRDevice(uint32_t deviceIndex);
    ~RTLSDRDevice();

    bool connect();
    void disconnect();

    bool setFrequency(uint32_t freqHz);
    bool setSampleRate(uint32_t rate);
    bool setGainMode(bool manual);
    bool setGain(uint32_t gainTenthsDb);
    bool setAGC(bool enable);
    size_t readIQ(uint8_t* buffer, size_t maxSamples);

private:
    static void asyncCallback(unsigned char* buf, uint32_t len, void* ctx);
    void asyncReadLoop();
    size_t availableBytesLocked() const;

    uint32_t m_deviceIndex;
    std::atomic<bool> m_connected;
    void* m_deviceHandle;
    std::vector<int> m_supportedGains;
    std::thread m_asyncThread;
    std::atomic<bool> m_asyncRunning;
    std::atomic<bool> m_asyncFailed;
    std::mutex m_bufferMutex;
    std::condition_variable m_bufferCv;
    std::vector<uint8_t> m_iqRing;
    size_t m_ringReadPos;
    size_t m_ringWritePos;
    bool m_ringFull;
};

#endif
