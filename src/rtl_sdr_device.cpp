#include "rtl_sdr_device.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>

#if defined(FM_TUNER_HAS_RTLSDR)
#include <rtl-sdr.h>

namespace {
const char* tunerTypeName(enum rtlsdr_tuner tuner) {
    switch (tuner) {
        case RTLSDR_TUNER_E4000:
            return "E4000";
        case RTLSDR_TUNER_FC0012:
            return "FC0012";
        case RTLSDR_TUNER_FC0013:
            return "FC0013";
        case RTLSDR_TUNER_FC2580:
            return "FC2580";
        case RTLSDR_TUNER_R820T:
            return "R820T";
        case RTLSDR_TUNER_R828D:
            return "R828D";
        default:
            return "unknown";
    }
}
}  // namespace
#endif

RTLSDRDevice::RTLSDRDevice(uint32_t deviceIndex)
    : m_deviceIndex(deviceIndex)
    , m_connected(false)
    , m_deviceHandle(nullptr)
    , m_asyncRunning(false)
    , m_asyncFailed(false)
    , m_iqRing(4U * 1024U * 1024U, 0)
    , m_ringReadPos(0)
    , m_ringWritePos(0)
    , m_ringFull(false) {
}

RTLSDRDevice::~RTLSDRDevice() {
    disconnect();
}

bool RTLSDRDevice::connect() {
#if defined(FM_TUNER_HAS_RTLSDR)
    if (m_connected) {
        return true;
    }
    const uint32_t count = rtlsdr_get_device_count();
    if (count == 0) {
        std::cerr << "[SDR] no RTL-SDR device found\n";
        return false;
    }
    if (m_deviceIndex >= count) {
        std::cerr << "[SDR] invalid RTL-SDR device index " << m_deviceIndex
                  << " (available: " << count << ")\n";
        return false;
    }
    rtlsdr_dev_t* dev = nullptr;
    if (rtlsdr_open(&dev, m_deviceIndex) != 0 || !dev) {
        std::cerr << "[SDR] failed to open RTL-SDR device index " << m_deviceIndex << "\n";
        return false;
    }
    std::cout << "[SDR] found " << tunerTypeName(rtlsdr_get_tuner_type(dev)) << " tuner\n";
    if (rtlsdr_reset_buffer(dev) != 0) {
        std::cerr << "[SDR] warning: failed to reset RTL-SDR buffer\n";
    }
    m_supportedGains.clear();
    const int gainCount = rtlsdr_get_tuner_gains(dev, nullptr);
    if (gainCount > 0) {
        m_supportedGains.resize(static_cast<size_t>(gainCount));
        if (rtlsdr_get_tuner_gains(dev, m_supportedGains.data()) <= 0) {
            m_supportedGains.clear();
        } else {
            std::sort(m_supportedGains.begin(), m_supportedGains.end());
        }
    }
    m_deviceHandle = dev;
    m_connected = true;
    m_asyncFailed = false;
    {
        std::lock_guard<std::mutex> lock(m_bufferMutex);
        m_ringReadPos = 0;
        m_ringWritePos = 0;
        m_ringFull = false;
    }
    m_asyncRunning = true;
    m_asyncThread = std::thread(&RTLSDRDevice::asyncReadLoop, this);
    return true;
#else
    std::cerr << "[SDR] direct RTL-SDR source not available in this build (enable FM_TUNER_ENABLE_RTLSDR)\n";
    return false;
#endif
}

void RTLSDRDevice::disconnect() {
#if defined(FM_TUNER_HAS_RTLSDR)
    m_asyncRunning = false;
    if (m_deviceHandle) {
        rtlsdr_cancel_async(reinterpret_cast<rtlsdr_dev_t*>(m_deviceHandle));
    }
    if (m_asyncThread.joinable()) {
        m_asyncThread.join();
    }
    if (m_deviceHandle) {
        rtlsdr_close(reinterpret_cast<rtlsdr_dev_t*>(m_deviceHandle));
        m_deviceHandle = nullptr;
    }
#endif
    {
        std::lock_guard<std::mutex> lock(m_bufferMutex);
        m_ringReadPos = 0;
        m_ringWritePos = 0;
        m_ringFull = false;
    }
    m_supportedGains.clear();
    m_connected = false;
}

bool RTLSDRDevice::setFrequency(uint32_t freqHz) {
#if defined(FM_TUNER_HAS_RTLSDR)
    if (!m_connected || !m_deviceHandle) {
        return false;
    }
    return rtlsdr_set_center_freq(reinterpret_cast<rtlsdr_dev_t*>(m_deviceHandle), freqHz) == 0;
#else
    (void)freqHz;
    return false;
#endif
}

bool RTLSDRDevice::setSampleRate(uint32_t rate) {
#if defined(FM_TUNER_HAS_RTLSDR)
    if (!m_connected || !m_deviceHandle) {
        return false;
    }
    return rtlsdr_set_sample_rate(reinterpret_cast<rtlsdr_dev_t*>(m_deviceHandle), rate) == 0;
#else
    (void)rate;
    return false;
#endif
}

bool RTLSDRDevice::setGainMode(bool manual) {
#if defined(FM_TUNER_HAS_RTLSDR)
    if (!m_connected || !m_deviceHandle) {
        return false;
    }
    return rtlsdr_set_tuner_gain_mode(reinterpret_cast<rtlsdr_dev_t*>(m_deviceHandle), manual ? 1 : 0) == 0;
#else
    (void)manual;
    return false;
#endif
}

bool RTLSDRDevice::setGain(uint32_t gainTenthsDb) {
#if defined(FM_TUNER_HAS_RTLSDR)
    if (!m_connected || !m_deviceHandle) {
        return false;
    }
    int gain = std::clamp(static_cast<int>(gainTenthsDb), 0, 500);
    if (!m_supportedGains.empty()) {
        auto it = std::lower_bound(m_supportedGains.begin(), m_supportedGains.end(), gain);
        if (it == m_supportedGains.begin()) {
            gain = *it;
        } else if (it == m_supportedGains.end()) {
            gain = m_supportedGains.back();
        } else {
            const int upper = *it;
            const int lower = *(it - 1);
            gain = ((upper - gain) < (gain - lower)) ? upper : lower;
        }
    }
    return rtlsdr_set_tuner_gain(reinterpret_cast<rtlsdr_dev_t*>(m_deviceHandle), gain) == 0;
#else
    (void)gainTenthsDb;
    return false;
#endif
}

bool RTLSDRDevice::setAGC(bool enable) {
#if defined(FM_TUNER_HAS_RTLSDR)
    if (!m_connected || !m_deviceHandle) {
        return false;
    }
    return rtlsdr_set_agc_mode(reinterpret_cast<rtlsdr_dev_t*>(m_deviceHandle), enable ? 1 : 0) == 0;
#else
    (void)enable;
    return false;
#endif
}

size_t RTLSDRDevice::readIQ(uint8_t* buffer, size_t maxSamples) {
#if defined(FM_TUNER_HAS_RTLSDR)
    if (!m_connected || !m_deviceHandle || !buffer || maxSamples == 0) {
        return 0;
    }
    const size_t requestedBytes = std::min<size_t>(maxSamples * 2, 1U << 20);
    std::unique_lock<std::mutex> lock(m_bufferMutex);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(35);
    while (m_connected && !m_asyncFailed.load()) {
        const size_t availableNow = availableBytesLocked();
        if (availableNow >= requestedBytes) {
            break;
        }
        if (availableNow > 0 && std::chrono::steady_clock::now() >= deadline) {
            break;
        }
        if (m_bufferCv.wait_until(lock, deadline) == std::cv_status::timeout) {
            break;
        }
    }
    size_t available = availableBytesLocked();
    if (available == 0) {
        return 0;
    }
    size_t bytesToRead = std::min(available, requestedBytes);
    bytesToRead &= ~static_cast<size_t>(1);
    if (bytesToRead == 0) {
        return 0;
    }
    size_t outByte = 0;
    size_t remaining = bytesToRead;
    while (remaining > 0) {
        const size_t head = m_iqRing.size() - m_ringReadPos;
        const size_t chunk = std::min(remaining, head);
        std::memcpy(buffer + outByte, m_iqRing.data() + m_ringReadPos, chunk);
        outByte += chunk;
        remaining -= chunk;
        m_ringReadPos += chunk;
        if (m_ringReadPos >= m_iqRing.size()) {
            m_ringReadPos = 0;
        }
    }
    m_ringFull = false;
    return outByte / 2;
#else
    (void)buffer;
    (void)maxSamples;
    return 0;
#endif
}

void RTLSDRDevice::asyncCallback(unsigned char* buf, uint32_t len, void* ctx) {
#if defined(FM_TUNER_HAS_RTLSDR)
    auto* self = reinterpret_cast<RTLSDRDevice*>(ctx);
    if (!self || !buf || len == 0 || self->m_iqRing.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(self->m_bufferMutex);
    static std::atomic<uint32_t> overflowCount{0};
    const size_t ringSize = self->m_iqRing.size();
    for (uint32_t i = 0; i < len; i++) {
        self->m_iqRing[self->m_ringWritePos] = buf[i];
        self->m_ringWritePos++;
        if (self->m_ringWritePos >= ringSize) {
            self->m_ringWritePos = 0;
        }
        if (self->m_ringFull) {
            self->m_ringReadPos++;
            if (self->m_ringReadPos >= ringSize) {
                self->m_ringReadPos = 0;
            }
            const uint32_t count = ++overflowCount;
            if (count <= 5 || (count % 1000) == 0) {
                std::cerr << "[SDR] IQ ring overflow (" << count << ")\n";
            }
        }
        self->m_ringFull = (self->m_ringWritePos == self->m_ringReadPos);
    }
    self->m_bufferCv.notify_one();
#else
    (void)buf;
    (void)len;
    (void)ctx;
#endif
}

void RTLSDRDevice::asyncReadLoop() {
#if defined(FM_TUNER_HAS_RTLSDR)
    auto* dev = reinterpret_cast<rtlsdr_dev_t*>(m_deviceHandle);
    if (!dev) {
        m_asyncRunning = false;
        m_asyncFailed = true;
        m_bufferCv.notify_all();
        return;
    }
    const int rc = rtlsdr_read_async(dev, &RTLSDRDevice::asyncCallback, this, 12, 16384);
    if (m_asyncRunning.load() && rc != 0) {
        std::cerr << "[SDR] read_async stopped with error rc=" << rc << "\n";
        m_asyncFailed = true;
    }
    m_asyncRunning = false;
    m_bufferCv.notify_all();
#endif
}

size_t RTLSDRDevice::availableBytesLocked() const {
    if (m_ringFull) {
        return m_iqRing.size();
    }
    if (m_ringWritePos >= m_ringReadPos) {
        return m_ringWritePos - m_ringReadPos;
    }
    return (m_iqRing.size() - m_ringReadPos) + m_ringWritePos;
}
