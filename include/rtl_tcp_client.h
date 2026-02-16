#ifndef RTL_TCP_CLIENT_H
#define RTL_TCP_CLIENT_H

#include <stdint.h>
#include <stddef.h>
#include <string>
#include <atomic>
#include <memory>

class RTLTCPClient {
public:
    RTLTCPClient(const std::string& host, uint16_t port);
    ~RTLTCPClient();

    bool connect();
    void disconnect();

    bool isConnected() const { return m_connected; }

    size_t readIQ(uint8_t* buffer, size_t maxSamples);

    bool setFrequency(uint32_t freqHz);
    bool setSampleRate(uint32_t rate);
    bool setGain(uint32_t gain);
    bool setAGC(bool enable);

    uint32_t getFrequency() const { return m_frequency; }
    uint32_t getSampleRate() const { return m_sampleRate; }

private:
    bool sendCommand(uint8_t cmd, const uint8_t* data, size_t len);
    bool readResponse(uint8_t* buffer, size_t len);

    std::string m_host;
    uint16_t m_port;
    int m_socket;
    std::atomic<bool> m_connected;
    std::atomic<uint32_t> m_frequency;
    std::atomic<uint32_t> m_sampleRate;
};

#endif
