#include "rtl_tcp_client.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <cstring>
#include <iostream>
#include <thread>
#include <mutex>

#define RTL_TCP_SYNC_BYTE 0xAA

RTLTCPClient::RTLTCPClient(const std::string& host, uint16_t port)
    : m_host(host)
    , m_port(port)
    , m_socket(-1)
    , m_connected(false)
    , m_frequency(0)
    , m_sampleRate(1024000) {
}

RTLTCPClient::~RTLTCPClient() {
    disconnect();
}

bool RTLTCPClient::connect() {
    m_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (m_socket < 0) {
        std::cerr << "Failed to create socket" << std::endl;
        return false;
    }

    struct sockaddr_in serverAddr;
    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(m_port);

    if (inet_pton(AF_INET, m_host.c_str(), &serverAddr.sin_addr) <= 0) {
        struct hostent* he = gethostbyname(m_host.c_str());
        if (!he) {
            std::cerr << "Invalid address: " << m_host << std::endl;
            close(m_socket);
            m_socket = -1;
            return false;
        }
        memcpy(&serverAddr.sin_addr, he->h_addr_list[0], he->h_length);
    }

    if (::connect(m_socket, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) < 0) {
        std::cerr << "Failed to connect to " << m_host << ":" << m_port << std::endl;
        close(m_socket);
        m_socket = -1;
        return false;
    }

    uint8_t header[12];
    ssize_t n = recv(m_socket, header, 12, MSG_PEEK);
    if (n > 0) {
        m_connected = true;
        return true;
    }

    std::cerr << "Invalid response from rtl_tcp server" << std::endl;
    close(m_socket);
    m_socket = -1;
    return false;
}

void RTLTCPClient::disconnect() {
    if (m_socket >= 0) {
        close(m_socket);
        m_socket = -1;
    }
    m_connected = false;
}

bool RTLTCPClient::sendCommand(uint8_t cmd, const uint8_t* data, size_t len) {
    if (!m_connected || m_socket < 0) {
        return false;
    }

    uint8_t buffer[64];
    buffer[0] = cmd;
    if (data && len > 0) {
        memcpy(&buffer[1], data, len);
    }

    ssize_t sent = send(m_socket, buffer, 1 + len, 0);
    return sent == (ssize_t)(1 + len);
}

bool RTLTCPClient::readResponse(uint8_t* buffer, size_t len) {
    if (!m_connected || m_socket < 0) {
        return false;
    }

    size_t totalRead = 0;
    while (totalRead < len) {
        ssize_t n = recv(m_socket, buffer + totalRead, len - totalRead, 0);
        if (n <= 0) {
            return false;
        }
        totalRead += n;
    }
    return true;
}

size_t RTLTCPClient::readIQ(uint8_t* buffer, size_t maxSamples) {
    if (!m_connected || m_socket < 0) {
        return 0;
    }

    size_t bytesToRead = maxSamples * 2;
    size_t totalRead = 0;

    while (totalRead < bytesToRead) {
        ssize_t n = recv(m_socket, buffer + totalRead, bytesToRead - totalRead, 0);
        if (n <= 0) {
            break;
        }
        totalRead += n;
    }

    return totalRead / 2;
}

bool RTLTCPClient::setFrequency(uint32_t freqHz) {
    uint32_t networkOrder = htonl(freqHz);
    uint8_t data[4];
    memcpy(data, &networkOrder, 4);

    if (sendCommand(0x01, data, 4)) {
        m_frequency = freqHz;
        return true;
    }
    return false;
}

bool RTLTCPClient::setSampleRate(uint32_t rate) {
    uint32_t networkOrder = htonl(rate);
    uint8_t data[4];
    memcpy(data, &networkOrder, 4);

    if (sendCommand(0x02, data, 4)) {
        m_sampleRate = rate;
        return true;
    }
    return false;
}

bool RTLTCPClient::setGain(uint32_t gain) {
    uint32_t networkOrder = htonl(gain);
    uint8_t data[4];
    memcpy(data, &networkOrder, 4);
    return sendCommand(0x04, data, 4);
}

bool RTLTCPClient::setAGC(bool enable) {
    uint8_t data[1];
    data[0] = enable ? 1 : 0;
    return sendCommand(0x08, data, 1);
}
