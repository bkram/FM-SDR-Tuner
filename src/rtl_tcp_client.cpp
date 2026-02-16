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
    if (readResponse(header, sizeof(header))) {
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

bool RTLTCPClient::sendAll(const uint8_t* data, size_t len) {
    if (!m_connected || m_socket < 0) {
        return false;
    }

    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(m_socket, data + sent, len - sent, 0);
        if (n <= 0) {
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

bool RTLTCPClient::sendCommand(uint8_t cmd, uint32_t param) {
    if (!m_connected || m_socket < 0) {
        return false;
    }

    uint8_t buffer[5];
    buffer[0] = cmd;
    uint32_t networkOrder = htonl(param);
    memcpy(&buffer[1], &networkOrder, sizeof(networkOrder));
    return sendAll(buffer, sizeof(buffer));
}

bool RTLTCPClient::readResponse(uint8_t* buffer, size_t len) {
    if (m_socket < 0) {
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
    if (sendCommand(0x01, freqHz)) {
        m_frequency = freqHz;
        return true;
    }
    return false;
}

bool RTLTCPClient::setSampleRate(uint32_t rate) {
    if (sendCommand(0x02, rate)) {
        m_sampleRate = rate;
        return true;
    }
    return false;
}

bool RTLTCPClient::setGainMode(bool manual) {
    return sendCommand(0x03, manual ? 1u : 0u);
}

bool RTLTCPClient::setGain(uint32_t gain) {
    return sendCommand(0x04, gain);
}

bool RTLTCPClient::setAGC(bool enable) {
    return sendCommand(0x08, enable ? 1u : 0u);
}
