#include "rtl_tcp_client.h"
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#endif
#include <cerrno>
#include <cstring>
#include <iostream>
#include <thread>
#include <mutex>

namespace {
void closeSocket(int sock) {
#if defined(_WIN32)
    closesocket(static_cast<SOCKET>(sock));
#else
    close(sock);
#endif
}

int lastSocketError() {
#if defined(_WIN32)
    return WSAGetLastError();
#else
    return errno;
#endif
}

bool socketInterrupted(int err) {
#if defined(_WIN32)
    return err == WSAEINTR;
#else
    return err == EINTR;
#endif
}

bool socketWouldBlock(int err) {
#if defined(_WIN32)
    return err == WSAEWOULDBLOCK || err == WSAETIMEDOUT;
#else
    return err == EAGAIN || err == EWOULDBLOCK;
#endif
}

void setRecvTimeoutMs(int sock, int timeoutMs) {
#if defined(_WIN32)
    const DWORD timeout = timeoutMs > 0 ? static_cast<DWORD>(timeoutMs) : 0;
    setsockopt(static_cast<SOCKET>(sock), SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
    struct timeval tv;
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
}

bool ensureSocketSubsystem() {
#if defined(_WIN32)
    static bool initialized = false;
    static bool success = false;
    if (!initialized) {
        WSADATA wsaData;
        success = (WSAStartup(MAKEWORD(2, 2), &wsaData) == 0);
        initialized = true;
    }
    return success;
#else
    return true;
#endif
}
}  // namespace

RTLTCPClient::RTLTCPClient(const std::string& host, uint16_t port)
    : m_host(host)
    , m_port(port)
    , m_socket(-1)
    , m_connected(false)
    , m_frequency(0)
    , m_sampleRate(1024000)
    , m_havePendingIqByte(false)
    , m_pendingIqByte(0) {
}

RTLTCPClient::~RTLTCPClient() {
    disconnect();
}

bool RTLTCPClient::connect() {
    if (!ensureSocketSubsystem()) {
        std::cerr << "Failed to initialize socket subsystem" << std::endl;
        return false;
    }

    struct addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    struct addrinfo* results = nullptr;
    const std::string portStr = std::to_string(m_port);
    const int gai = getaddrinfo(m_host.c_str(), portStr.c_str(), &hints, &results);
    if (gai != 0 || !results) {
        std::cerr << "Invalid address: " << m_host
                  << " (" << gai_strerror(gai) << ")" << std::endl;
        return false;
    }

    m_socket = -1;
    for (struct addrinfo* ai = results; ai != nullptr; ai = ai->ai_next) {
        const int sock = static_cast<int>(socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol));
        if (sock < 0) {
            continue;
        }
        if (::connect(sock, ai->ai_addr, ai->ai_addrlen) == 0) {
            m_socket = sock;
            break;
        }
        closeSocket(sock);
    }
    freeaddrinfo(results);

    if (m_socket < 0) {
        std::cerr << "Failed to connect to " << m_host << ":" << m_port << std::endl;
        return false;
    }

    // Guard initial rtl_tcp header read so connect() cannot block forever.
    setRecvTimeoutMs(m_socket, 2000);

    uint8_t header[12];
    if (readResponse(header, sizeof(header))) {
        setRecvTimeoutMs(m_socket, 2000);
        m_havePendingIqByte = false;
        m_connected = true;
        return true;
    }

    std::cerr << "Invalid response from rtl_tcp server" << std::endl;
    closeSocket(m_socket);
    m_socket = -1;
    return false;
}

void RTLTCPClient::disconnect() {
    if (m_socket >= 0) {
        closeSocket(m_socket);
        m_socket = -1;
    }
    m_connected = false;
    m_havePendingIqByte = false;
}

bool RTLTCPClient::sendAll(const uint8_t* data, size_t len) {
    if (!m_connected || m_socket < 0) {
        return false;
    }

    size_t sent = 0;
    while (sent < len) {
        const auto n = send(m_socket, reinterpret_cast<const char*>(data + sent),
                            static_cast<int>(len - sent), 0);
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
        const auto n = recv(m_socket, reinterpret_cast<char*>(buffer + totalRead),
                            static_cast<int>(len - totalRead), 0);
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
    if (bytesToRead == 0) {
        return 0;
    }
    size_t totalRead = 0;
    if (m_havePendingIqByte) {
        buffer[0] = m_pendingIqByte;
        m_havePendingIqByte = false;
        totalRead = 1;
    }

    while (totalRead < bytesToRead) {
        const auto n = recv(m_socket, reinterpret_cast<char*>(buffer + totalRead),
                            static_cast<int>(bytesToRead - totalRead), 0);
        if (n > 0) {
            totalRead += static_cast<size_t>(n);
            continue;
        }
        if (n == 0) {
            m_connected = false;
            break;
        }
        const int err = lastSocketError();
        if (socketInterrupted(err)) {
            continue;
        }
        if (socketWouldBlock(err)) {
            break;
        }
        break;
    }

    if ((totalRead & 1u) != 0u) {
        m_pendingIqByte = buffer[totalRead - 1];
        m_havePendingIqByte = true;
        totalRead--;
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
