#include "xdr_server.h"
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif
#include <cstring>
#include <iostream>
#include <sstream>
#include <algorithm>
#include <cstdio>
#include <chrono>
#include <cerrno>
#include <iomanip>
#include <cmath>
#include <openssl/sha.h>
#include <openssl/rand.h>
#include <random>

namespace {
#if defined(_WIN32)
using SocketLen = int;
#else
using SocketLen = socklen_t;
#endif

uint64_t steadyNowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

void setRecvTimeoutMs(int clientSocket, int timeoutMs) {
#if defined(_WIN32)
    const DWORD timeout = timeoutMs > 0 ? static_cast<DWORD>(timeoutMs) : 0;
    setsockopt(clientSocket, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
    struct timeval tv;
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    setsockopt(clientSocket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
}

void closeSocket(int sock) {
#if defined(_WIN32)
    closesocket(static_cast<SOCKET>(sock));
#else
    close(sock);
#endif
}

void shutdownSocket(int sock) {
#if defined(_WIN32)
    shutdown(static_cast<SOCKET>(sock), SD_BOTH);
#else
    shutdown(sock, SHUT_RDWR);
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

bool equalsIgnoreCase(const std::string& a, const std::string& b) {
#if defined(_WIN32)
    return _stricmp(a.c_str(), b.c_str()) == 0;
#else
    return strcasecmp(a.c_str(), b.c_str()) == 0;
#endif
}

bool recvLine(int clientSocket, std::string& line, size_t maxLen) {
    line.clear();

    while (line.length() < maxLen) {
        char ch = '\0';
        const auto n = recv(clientSocket, &ch, 1, 0);
        if (n <= 0) {
            return !line.empty();
        }

        if (ch == '\n') {
            return true;
        }

        if (ch != '\r') {
            line.push_back(ch);
        }
    }

    return true;
}

bool parseIntValue(const std::string& arg, int& out) {
    if (arg.empty()) {
        return false;
    }
    try {
        size_t parsed = 0;
        int value = std::stoi(arg, &parsed);
        if (parsed != arg.size()) {
            return false;
        }
        out = value;
        return true;
    } catch (...) {
        return false;
    }
}

bool parseTefCustomValue(const std::string& arg, int& out) {
    if (arg.size() != 2) {
        return false;
    }
    if ((arg[0] != '0' && arg[0] != '1') ||
        (arg[1] != '0' && arg[1] != '1')) {
        return false;
    }
    out = (arg[0] - '0') * 10 + (arg[1] - '0');
    return true;
}

bool parseFrequencyHz(const std::string& arg, uint32_t& outHz) {
    if (arg.empty()) {
        return false;
    }
    try {
        size_t parsed = 0;
        unsigned long long raw = std::stoull(arg, &parsed);
        if (parsed != arg.size() || raw == 0) {
            return false;
        }
        // xdr-gtk uses kHz; some clients use Hz. Accept both.
        if (raw < 1000000ULL) {
            raw *= 1000ULL;
        }
        if (raw > 0xFFFFFFFFULL) {
            return false;
        }
        outHz = static_cast<uint32_t>(raw);
        return true;
    } catch (...) {
        return false;
    }
}

std::string formatGain(int gain) {
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "G%02d", gain);
    return std::string(buffer);
}

std::string formatSampling(int interval, int detector) {
    std::ostringstream oss;
    oss << "I" << interval << "," << detector;
    return oss.str();
}

uint8_t evaluatePiState(const std::array<uint16_t, 64>& piBuffer,
                        const std::array<uint8_t, 8>& piErrorBuffer,
                        uint8_t fill,
                        uint16_t value) {
    uint8_t count = 0;
    uint8_t correctCount = 0;

    for (uint8_t i = 0; i < fill; i++) {
        if (piBuffer[i] == value) {
            count++;
            if ((piErrorBuffer[i / 8] & (1 << (i % 8))) == 0) {
                correctCount++;
            }
        }
    }

    if (correctCount >= 2) return 0;       // STATE_CORRECT
    if (count >= 2 && correctCount) return 1;  // STATE_VERY_LIKELY
    if (count >= 3) return 2;              // STATE_LIKELY
    if (count == 2 || correctCount) return 3;  // STATE_UNLIKELY
    return 4;                              // STATE_INVALID
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

void XDRServer::addClientSocket(int clientSocket) {
    std::lock_guard<std::mutex> lock(m_clientSocketsMutex);
    m_clientSockets.push_back(clientSocket);
}

void XDRServer::removeClientSocket(int clientSocket) {
    std::lock_guard<std::mutex> lock(m_clientSocketsMutex);
    auto it = std::find(m_clientSockets.begin(), m_clientSockets.end(), clientSocket);
    if (it != m_clientSockets.end()) {
        m_clientSockets.erase(it);
    }
}

XDRServer::XDRServer(uint16_t port)
    : m_port(port)
    , m_serverSocket(-1)
    , m_running(false)
    , m_activeClientThreads(0)
    , m_guestMode(false)
    , m_verboseLogging(true)
    , m_mode(0)
    , m_frequency(88500000)
    , m_volume(100)
    , m_deemphasis(0)
    , m_filter(-1)
    , m_bandwidth(0)
    , m_antenna(0)
    , m_gain(0)
    , m_agcMode(2)
    , m_daa(0)
    , m_squelch(0)
    , m_rotator(0)
    , m_samplingInterval(66)
    , m_detector(0)
    , m_forceMono(false)
    , m_signalDeci(0)
    , m_signalStereo(false)
    , m_signalForcedMono(false)
    , m_cci(-1)
    , m_aci(-1)
    , m_pilotTenthsKHz(0) {
    m_scanStartKHz = 87500;
    m_scanStopKHz = 108000;
    m_scanStepKHz = 100;
    m_scanBandwidthHz = 0;
    m_scanAntenna = 0;
    m_scanContinuous = false;
    m_scanStartPending = false;
    m_scanCancelPending = false;
    m_piFill = 0;
    m_piPos = 63;
    m_piBuffer.fill(0);
    m_piErrorBuffer.fill(0);
    m_piLastState = 4;
    m_piLastValue = 0xFFFF;
    m_lastRdsMs = 0;
}

XDRServer::~XDRServer() {
    stop();
}

void XDRServer::setPassword(const std::string& password) {
    m_password = password;
}

void XDRServer::setGuestMode(bool enabled) {
    m_guestMode = enabled;
}

void XDRServer::setVerboseLogging(bool enabled) {
    m_verboseLogging = enabled;
}

std::string XDRServer::generateSalt() {
    static const char chars[] = "QWERTYUIOPASDFGHJKLZXCVBNMqwertyuiopasdfghjklzxcvbnm0123456789_-";
    constexpr size_t len = sizeof(chars) - 1;
    unsigned char random_data[SALT_LENGTH];
    
    if (!RAND_bytes(random_data, sizeof(random_data))) {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<size_t> dis(0, len - 1);
        std::string salt;
        for (int i = 0; i < SALT_LENGTH; i++) {
            salt += chars[dis(gen)];
        }
        return salt;
    }
    
    std::string salt;
    for (int i = 0; i < SALT_LENGTH; i++) {
        salt += chars[static_cast<size_t>(random_data[i]) % len];
    }
    return salt;
}

std::string XDRServer::computeSHA1(const std::string& salt, const std::string& password) {
    unsigned char sha[SHA_DIGEST_LENGTH];
    SHA_CTX ctx;
    
    SHA1_Init(&ctx);
    SHA1_Update(&ctx, salt.c_str(), salt.length());
    SHA1_Update(&ctx, password.c_str(), password.length());
    SHA1_Final(sha, &ctx);
    
    char sha_string[SHA_DIGEST_LENGTH * 2 + 1];
    for (int i = 0; i < SHA_DIGEST_LENGTH; i++) {
        std::snprintf(sha_string + (i * 2), 3, "%02x", sha[i]);
    }
    sha_string[SHA_DIGEST_LENGTH * 2] = '\0';
    return std::string(sha_string);
}

bool XDRServer::authenticate(const std::string& salt, const std::string& passwordHash) {
    std::string expected = computeSHA1(salt, m_password);
    return (expected.length() == passwordHash.length() &&
            equalsIgnoreCase(expected, passwordHash));
}

std::string XDRServer::buildXdrStateSnapshot() const {
    std::ostringstream oss;
    oss << "M" << m_mode << "\n"
        << "Y" << m_volume << "\n"
        << "T" << (m_frequency / 1000) << "\n"
        << "D" << m_deemphasis << "\n"
        << "A" << m_agcMode << "\n"
        << "W" << m_bandwidth << "\n";

    oss << "Z" << m_antenna << "\n"
        << formatGain(m_gain) << "\n"
        << "V" << m_daa << "\n"
        << "Q" << m_squelch << "\n"
        << "C" << m_rotator << "\n"
        << formatSampling(m_samplingInterval, m_detector);
    return oss.str();
}

std::string XDRServer::buildSignalLine() const {
    const bool forcedMono = m_signalForcedMono.load();
    const bool stereo = m_signalStereo.load();
    const int cci = m_cci.load();
    const int aci = m_aci.load();
    const double level = m_signalDeci.load() / 10.0;

    char mode = 'm';
    if (forcedMono) {
        mode = stereo ? 'S' : 'M';
    } else if (stereo) {
        mode = 's';
    }

    std::ostringstream oss;
    oss << "S" << mode << std::fixed << std::setprecision(1) << level
        << "," << cci << "," << aci;
    return oss.str();
}

void XDRServer::updateSignal(float level, bool stereo, bool forcedMono, int cci, int aci) {
    const int deci = static_cast<int>(std::round(std::clamp(level, 0.0f, 120.0f) * 10.0f));
    m_signalDeci = deci;
    m_signalStereo = stereo;
    m_signalForcedMono = forcedMono;
    m_cci = cci;
    m_aci = aci;
}

void XDRServer::updatePilot(int pilotTenthsKHz) {
    m_pilotTenthsKHz = std::clamp(pilotTenthsKHz, 0, 750);
}

void XDRServer::updateRDS(uint16_t blockA, uint16_t blockB, uint16_t blockC, uint16_t blockD, uint8_t errors) {
    m_lastRdsMs = steadyNowMs();

    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "R%04X%04X%04X%02X", blockB, blockC, blockD, errors);
    std::string rLine(buffer);
    
    std::lock_guard<std::mutex> lock(m_rdsMutex);
    
    constexpr size_t kMaxRdsQueue = 256;

    const uint8_t blockAErr = static_cast<uint8_t>((errors >> 6) & 0x03u);
    
    m_piPos = (m_piPos + 1) % 64;
    m_piBuffer[m_piPos] = blockA;
    
    const uint8_t errPos = m_piPos / 8;
    const uint8_t errBit = m_piPos % 8;
    if (blockAErr != 0) {
        m_piErrorBuffer[errPos] |= (1 << errBit);
    } else {
        m_piErrorBuffer[errPos] &= ~(1 << errBit);
    }
    
    if (m_piFill < 64) {
        m_piFill++;
    }

    const uint8_t piState = evaluatePiState(m_piBuffer, m_piErrorBuffer, m_piFill, blockA);
    const bool blockAPresent = (blockAErr != 3);
    const bool piDebounced = blockAPresent && (piState <= 1);

    if (piDebounced) {
        char piBuffer[16];
        std::snprintf(piBuffer,
                      sizeof(piBuffer),
                      "P%04X%.*s",
                      blockA,
                      static_cast<int>(std::min<uint8_t>(blockAErr, 3)),
                      "???");
        if (m_rdsQueue.size() >= kMaxRdsQueue) {
            m_rdsQueue.pop_front();
        }
        m_rdsQueue.emplace_back(m_rdsNextSeq++, std::string(piBuffer));
        m_piLastValue = blockA;
    }

    if (m_rdsQueue.size() >= kMaxRdsQueue) {
        m_rdsQueue.pop_front();
    }
    m_rdsQueue.emplace_back(m_rdsNextSeq++, rLine);
    m_piLastState = piState;
}

void XDRServer::setFrequencyState(uint32_t freqHz) {
    if (freqHz == 0) {
        return;
    }
    if (freqHz != m_frequency) {
        std::lock_guard<std::mutex> lock(m_rdsMutex);
        m_piFill = 0;
        m_piPos = 63;
        m_piBuffer.fill(0);
        m_piErrorBuffer.fill(0);
        m_piLastState = 4;
        m_piLastValue = 0xFFFF;
    }
    m_frequency = freqHz;
}

bool XDRServer::consumeScanStart(ScanConfig& config) {
    if (!m_scanStartPending.exchange(false)) {
        return false;
    }
    config.startKHz = m_scanStartKHz.load();
    config.stopKHz = m_scanStopKHz.load();
    config.stepKHz = m_scanStepKHz.load();
    config.bandwidthHz = m_scanBandwidthHz.load();
    config.antenna = m_scanAntenna.load();
    config.continuous = m_scanContinuous.load();
    return true;
}

bool XDRServer::consumeScanCancel() {
    return m_scanCancelPending.exchange(false);
}

void XDRServer::pushScanLine(const std::string& line) {
    if (line.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(m_scanMutex);
    if (m_scanQueue.size() >= 8) {
        m_scanQueue.pop_front();
    }
    m_scanQueue.emplace_back(m_scanNextSeq++, "U" + line);
}

bool XDRServer::start() {
    if (m_running) {
        return false;
    }

    if (!ensureSocketSubsystem()) {
        std::cerr << "[XDR] failed to initialize socket subsystem" << std::endl;
        return false;
    }

    m_serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (m_serverSocket < 0) {
        std::cerr << "[XDR] failed to create server socket" << std::endl;
        return false;
    }

    int opt = 1;
#if defined(_WIN32)
    setsockopt(m_serverSocket, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&opt), static_cast<int>(sizeof(opt)));
#else
    setsockopt(m_serverSocket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif

    struct sockaddr_in serverAddr;
    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port = htons(m_port);

    if (bind(m_serverSocket, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) < 0) {
        std::cerr << "[XDR] failed to bind to port " << m_port << std::endl;
        closeSocket(m_serverSocket);
        m_serverSocket = -1;
        return false;
    }

    if (listen(m_serverSocket, 5) < 0) {
        std::cerr << "[XDR] failed to listen on port " << m_port << std::endl;
        closeSocket(m_serverSocket);
        m_serverSocket = -1;
        return false;
    }

    m_running = true;
    m_acceptThread = std::thread([this]() {
        while (m_running) {
            struct sockaddr_in clientAddr;
            SocketLen clientLen = sizeof(clientAddr);
            int clientSocket = accept(m_serverSocket, (struct sockaddr*)&clientAddr, &clientLen);

            if (clientSocket >= 0) {
                addClientSocket(clientSocket);
                m_activeClientThreads.fetch_add(1, std::memory_order_relaxed);
                std::thread([this, clientSocket]() {
                    handleClient(clientSocket);
                    removeClientSocket(clientSocket);
                    closeSocket(clientSocket);
                    m_activeClientThreads.fetch_sub(1, std::memory_order_relaxed);
                    m_clientThreadWaitCv.notify_all();
                }).detach();
            } else if (m_running && socketInterrupted(lastSocketError())) {
                continue;
            }
        }
    });

    if (m_verboseLogging.load()) {
        std::cout << "[XDR] listening on port " << m_port << std::endl;
    }
    return true;
}

void XDRServer::stop() {
    if (!m_running) {
        return;
    }

    m_running = false;

    if (m_serverSocket >= 0) {
        shutdownSocket(m_serverSocket);
        closeSocket(m_serverSocket);
        m_serverSocket = -1;
    }

    std::vector<int> clients;
    {
        std::lock_guard<std::mutex> lock(m_clientSocketsMutex);
        clients = m_clientSockets;
    }
    for (int sock : clients) {
        shutdownSocket(sock);
    }

    if (m_acceptThread.joinable()) {
        m_acceptThread.join();
    }

    std::unique_lock<std::mutex> lock(m_clientThreadWaitMutex);
    m_clientThreadWaitCv.wait_for(lock, std::chrono::seconds(2), [&]() {
        return m_activeClientThreads.load(std::memory_order_relaxed) == 0;
    });
}

void XDRServer::handleClient(int clientSocket) {
    char clientIP[INET_ADDRSTRLEN];
    struct sockaddr_in clientAddr;
    SocketLen clientLen = sizeof(clientAddr);
    getpeername(clientSocket, (struct sockaddr*)&clientAddr, &clientLen);
    inet_ntop(AF_INET, &clientAddr.sin_addr, clientIP, INET_ADDRSTRLEN);
    
    std::cout << "[XDR] client connected from " << clientIP << std::endl;
    
    setRecvTimeoutMs(clientSocket, 200);
    char firstByte = '\0';
    const auto n = recv(clientSocket, &firstByte, 1, MSG_PEEK);
    setRecvTimeoutMs(clientSocket, 0);

    bool isFmdxProtocol = (n > 0 && firstByte == 'x');
    
    if (isFmdxProtocol) {
        // Consume the initial FM-DX handshake line ("x") before command mode.
        std::string handshake;
        if (!recvLine(clientSocket, handshake, 16) || handshake != "x") {
            if (m_verboseLogging.load()) {
                std::cout << "[XDR] invalid FM-DX handshake payload" << std::endl;
            }
            std::cout << "[XDR] client disconnected from " << clientIP << std::endl;
            return;
        }
        if (m_verboseLogging.load()) {
            std::cout << "[XDR] detected FM-DX protocol handshake" << std::endl;
        }
        send(clientSocket, "1\n", 2, 0);
        handleFmdxClient(clientSocket);
    } else {
        if (m_verboseLogging.load()) {
            std::cout << "[XDR] detected XDR protocol handshake" << std::endl;
        }
        handleXdrClient(clientSocket, clientIP);
    }

    std::cout << "[XDR] client disconnected from " << clientIP << std::endl;
}

void XDRServer::handleFmdxClient(int clientSocket) {
    bool tunerStarted = false;
    bool disconnectRequested = false;
    char cmdBuffer[256];
    std::string command;
    setRecvTimeoutMs(clientSocket, 200);

    while (m_running && !disconnectRequested) {
        const auto n = recv(clientSocket, cmdBuffer, sizeof(cmdBuffer) - 1, 0);
        if (n < 0) {
            if (socketWouldBlock(lastSocketError())) {
                continue;
            }
            break;
        }
        if (n == 0) {
            break;
        }

        cmdBuffer[n] = '\0';
        command += cmdBuffer;

        size_t pos;
        while ((pos = command.find('\n')) != std::string::npos) {
            std::string line = command.substr(0, pos);
            command = command.substr(pos + 1);

            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }

            if (line == "x") {
                tunerStarted = true;
                if (m_startCallback) {
                    m_startCallback();
                }
                send(clientSocket, "1\n", 2, 0);
            } else if (line == "X") {
                tunerStarted = false;
                if (m_stopCallback) {
                    m_stopCallback();
                }
                send(clientSocket, "1\n", 2, 0);
            } else if (tunerStarted) {
                std::string response = processFmdxCommand(line);
                response += "\n";
                if (send(clientSocket, response.c_str(), response.length(), 0) <= 0) {
                    disconnectRequested = true;
                    break;
                }
            } else {
                if (send(clientSocket, "ER\n", 3, 0) <= 0) {
                    disconnectRequested = true;
                    break;
                }
            }
        }
    }
    setRecvTimeoutMs(clientSocket, 0);

    if (tunerStarted && m_stopCallback) {
        m_stopCallback();
    }
}

void XDRServer::handleXdrClient(int clientSocket, const char* clientIP) {
    if (m_verboseLogging.load()) {
        std::cout << "[XDR] client authenticating from " << clientIP << std::endl;
    }
    bool authenticated = false;
    bool guestSession = false;
    
    std::string salt = generateSalt();
    
    if (m_verboseLogging.load()) {
        std::cout << "[XDR] sending salt: '" << salt << "'" << std::endl;
    }
    
    std::string msg = salt + "\n";
    send(clientSocket, msg.c_str(), msg.length(), 0);
    
    std::string authMsg;
    if (!recvLine(clientSocket, authMsg, HASH_LENGTH + 2)) {
        if (m_verboseLogging.load()) {
            std::cout << "[XDR] client disconnected before sending auth hash" << std::endl;
        }
        return;
    }

    std::string clientHash = authMsg;
    if (clientHash.length() == HASH_LENGTH + 1 &&
        (clientHash[0] == 'P' || clientHash[0] == 'p')) {
        clientHash = clientHash.substr(1);
    }
    
    if (m_verboseLogging.load()) {
        std::cout << "[XDR] received auth payload: '" << authMsg << "' -> hash '" << clientHash
                  << "' (length: " << clientHash.length() << ")" << std::endl;
    }
    
    bool authSuccess = authenticate(salt, clientHash);
    if (m_verboseLogging.load()) {
        std::cout << "[XDR] authentication " << (authSuccess ? "SUCCESS" : "FAILED") << std::endl;
    }
    
    if (!authSuccess && !m_guestMode) {
        send(clientSocket, "a0\n", 3, 0);
        return;
    }
    
    if (!authSuccess && m_guestMode) {
        send(clientSocket, "a1\n", 3, 0);
        guestSession = true;
    } else {
        send(clientSocket, "a2\n", 3, 0);
        guestSession = false;
    }
    
    authenticated = authSuccess || m_guestMode;
    uint64_t lastRdsSeq = 0;
    uint64_t lastScanSeq = 0;
    if (authenticated) {
        {
            std::lock_guard<std::mutex> lock(m_rdsMutex);
            if (!m_rdsQueue.empty()) {
                lastRdsSeq = m_rdsQueue.back().first;
            }
        }
        {
            std::lock_guard<std::mutex> lock(m_scanMutex);
            if (!m_scanQueue.empty()) {
                lastScanSeq = m_scanQueue.back().first;
            }
        }
        std::string online = guestSession ? "o0,1\n" : "o1,0\n";
        send(clientSocket, online.c_str(), online.length(), 0);

        std::string snapshot = buildXdrStateSnapshot();
        snapshot += "\n";
        send(clientSocket, snapshot.c_str(), snapshot.length(), 0);
    }
    
    char cmdBuffer[256];
    std::string command;
    setRecvTimeoutMs(clientSocket, 100);
    auto lastSignal = std::chrono::steady_clock::now();

    while (m_running) {
        const auto n = recv(clientSocket, cmdBuffer, sizeof(cmdBuffer) - 1, 0);
        if (n < 0) {
            if (socketWouldBlock(lastSocketError())) {
                bool sendFailed = false;
                std::vector<std::string> rdsLines;
                {
                    std::lock_guard<std::mutex> lock(m_rdsMutex);
                    for (const auto& entry : m_rdsQueue) {
                        if (entry.first > lastRdsSeq) {
                            rdsLines.push_back(entry.second);
                            lastRdsSeq = entry.first;
                        }
                    }
                }
                for (const std::string& rdsLineBase : rdsLines) {
                    std::string rdsLine = rdsLineBase + "\n";
                    if (send(clientSocket, rdsLine.c_str(), rdsLine.length(), 0) <= 0) {
                        sendFailed = true;
                        break;
                    }
                }
                if (sendFailed) {
                    break;
                }

                std::vector<std::string> scanLines;
                {
                    std::lock_guard<std::mutex> lock(m_scanMutex);
                    for (const auto& entry : m_scanQueue) {
                        if (entry.first > lastScanSeq) {
                            scanLines.push_back(entry.second);
                            lastScanSeq = entry.first;
                        }
                    }
                }
                for (const std::string& scanLineBase : scanLines) {
                    std::string scanLine = scanLineBase + "\n";
                    if (send(clientSocket, scanLine.c_str(), scanLine.length(), 0) <= 0) {
                        sendFailed = true;
                        break;
                    }
                }
                if (sendFailed) {
                    break;
                }

                int intervalMs = m_samplingInterval.load();
                if (intervalMs <= 0) {
                    intervalMs = 66;
                }
                auto now = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastSignal).count();
                if (elapsed >= intervalMs) {
                    const uint64_t lastRdsMs = m_lastRdsMs.load(std::memory_order_relaxed);
                    const uint64_t nowMs = steadyNowMs();
                    if (lastRdsMs != 0 && nowMs - lastRdsMs <= 1500) {
                        uint16_t piValue = 0xFFFF;
                        {
                            std::lock_guard<std::mutex> lock(m_rdsMutex);
                            piValue = m_piLastValue;
                        }
                        if (piValue != 0xFFFF) {
                            char piLine[16];
                            std::snprintf(piLine, sizeof(piLine), "P%04X\n", piValue);
                            if (send(clientSocket, piLine, std::strlen(piLine), 0) <= 0) {
                                break;
                            }
                        }
                    }

                    std::string signalMsg = buildSignalLine();
                    signalMsg += "\n";
                    if (send(clientSocket, signalMsg.c_str(), signalMsg.length(), 0) <= 0) {
                        break;
                    }
                    lastSignal = now;
                }
                continue;
            }
            break;
        }
        if (n == 0) {
            break;
        }

        cmdBuffer[n] = '\0';
        command += cmdBuffer;

        size_t pos;
        while ((pos = command.find('\n')) != std::string::npos) {
            std::string line = command.substr(0, pos);
            command = command.substr(pos + 1);

            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }

            std::string response = processCommand(line, authenticated, guestSession);
            if (!response.empty()) {
                response += "\n";
                send(clientSocket, response.c_str(), response.length(), 0);
            }
        }
    }
    setRecvTimeoutMs(clientSocket, 0);

    m_scanCancelPending = true;
}

std::string XDRServer::processCommand(const std::string& cmd, bool authenticated, bool guestSession) {
    if (cmd.empty()) {
        m_scanCancelPending = true;
        return "";
    }

    if (!authenticated) {
        return "a0";
    }

    char command = cmd[0];
    std::string arg = cmd.length() > 1 ? cmd.substr(1) : "";

    std::lock_guard<std::mutex> lock(m_callbackMutex);

    switch (command) {
        case 'x': {
            if (m_startCallback) {
                m_startCallback();
            }
            return "OK";
        }

        case 'X': {
            if (m_stopCallback) {
                m_stopCallback();
            }
            return "X";
        }

        case 'P':
            return "a2";

        case 'S':
            // TEF-style scan control:
            // Sa<start_kHz> Sb<stop_kHz> Sc<step_kHz> Sw<bw_hz> Sz<antenna> S / Sm
            if (arg.empty()) {
                m_scanContinuous = false;
                m_scanStartPending = true;
                return "";
            }
            if (arg == "m") {
                m_scanContinuous = true;
                m_scanStartPending = true;
                return "";
            }
            if (arg.size() >= 2) {
                const char sub = arg[0];
                int value = 0;
                if (parseIntValue(arg.substr(1), value)) {
                    switch (sub) {
                        case 'a':
                            m_scanStartKHz = std::clamp(value, 64000, 120000);
                            break;
                        case 'b':
                            m_scanStopKHz = std::clamp(value, 64000, 120000);
                            break;
                        case 'c':
                            m_scanStepKHz = std::clamp(value, 5, 1000);
                            break;
                        case 'w':
                            m_scanBandwidthHz = std::clamp(value, 0, 400000);
                            break;
                        case 'z':
                            m_scanAntenna = std::clamp(value, 0, 9);
                            break;
                        case 'f':
                            break;
                        default:
                            break;
                    }
                }
            }
            return "";

        case 'T': {
            uint32_t freqHz = 0;
            if (!parseFrequencyHz(arg, freqHz)) {
                return "";
            }
            m_frequency = freqHz;
            if (m_freqCallback) {
                m_freqCallback(freqHz);
            }
            return "T" + std::to_string(freqHz / 1000);
        }

        case 'Y': {
            int volume = 0;
            if (!parseIntValue(arg, volume)) {
                return "";
            }
            volume = std::clamp(volume, 0, 100);
            m_volume = volume;
            if (m_volCallback) {
                m_volCallback(volume);
            }
            return "Y" + std::to_string(volume);
        }

        case 'V': {
            int daa = 0;
            if (!parseIntValue(arg, daa)) {
                return "";
            }
            // TEF alignment: 6 dB steps up to 36 dB.
            daa = std::clamp(daa, 0, 36);
            daa = ((daa + 3) / 6) * 6;
            daa = std::clamp(daa, 0, 36);
            m_daa = daa;
            if (m_alignmentCallback) {
                m_alignmentCallback(daa);
            }
            return "V" + std::to_string(daa);
        }

        case 'G': {
            int gain = 0;
            if (!parseTefCustomValue(arg, gain)) {
                return "";
            }
            bool accepted = true;
            if (m_gainCallback) {
                accepted = m_gainCallback(gain);
            }
            if (!accepted) {
                return formatGain(m_gain);
            }
            m_gain = gain;
            return formatGain(gain);
        }

        case 'A': {
            int agcMode = 0;
            if (!parseIntValue(arg, agcMode)) {
                return "";
            }
            agcMode = std::clamp(agcMode, 0, 3);
            bool accepted = true;
            if (m_agcCallback) {
                accepted = m_agcCallback(agcMode);
            }
            if (!accepted) {
                return "A" + std::to_string(m_agcMode.load());
            }
            m_agcMode = agcMode;
            return "A" + std::to_string(agcMode);
        }

        case 'M': {
            int mode = 0;
            if (!parseIntValue(arg, mode)) {
                return "";
            }
            mode = std::clamp(mode, 0, 1);
            m_mode = mode;
            if (m_modeCallback) {
                m_modeCallback(mode);
            }
            return "M" + std::to_string(mode);
        }

        case 'D': {
            int deemph = 0;
            if (!parseIntValue(arg, deemph)) {
                return "";
            }
            deemph = std::clamp(deemph, 0, 2);
            m_deemphasis = deemph;
            if (m_deemphasisCallback) {
                m_deemphasisCallback(deemph);
            }
            return "D" + std::to_string(deemph);
        }

        case 'F':
            // TEF-style backends report bandwidth using 'W'.
            return "";

        case 'W': {
            int bandwidth = 0;
            if (!parseIntValue(arg, bandwidth)) {
                return "";
            }
            m_bandwidth = bandwidth;
            if (m_bandwidthCallback) {
                m_bandwidthCallback(bandwidth);
            }
            return "W" + std::to_string(bandwidth);
        }

        case 'Z': {
            int antenna = 0;
            if (!parseIntValue(arg, antenna)) {
                return "";
            }
            antenna = std::clamp(antenna, 0, 9);
            m_antenna = antenna;
            if (m_antennaCallback) {
                m_antennaCallback(antenna);
            }
            return "Z" + std::to_string(antenna);
        }

        case 'Q': {
            int squelch = 0;
            if (!parseIntValue(arg, squelch)) {
                return "";
            }
            squelch = std::clamp(squelch, 0, 100);
            m_squelch = squelch;
            if (m_squelchCallback) {
                m_squelchCallback(squelch);
            }
            return "Q" + std::to_string(squelch);
        }

        case 'C': {
            int rotator = 0;
            if (!parseIntValue(arg, rotator)) {
                return "";
            }
            rotator = std::clamp(rotator, -1, 1);
            m_rotator = rotator;
            if (m_rotatorCallback) {
                m_rotatorCallback(rotator);
            }
            return "C" + std::to_string(rotator);
        }

        case 'I': {
            if (arg.empty()) {
                return formatSampling(m_samplingInterval, m_detector);
            }

            int interval = 0;
            int detector = m_detector;
            size_t comma = arg.find(',');
            if (comma == std::string::npos) {
                if (!parseIntValue(arg, interval)) {
                    return "";
                }
            } else {
                if (!parseIntValue(arg.substr(0, comma), interval) ||
                    !parseIntValue(arg.substr(comma + 1), detector)) {
                    return "";
                }
            }

            interval = std::clamp(interval, 0, 1000);
            detector = std::clamp(detector, 0, 1);
            m_samplingInterval = interval;
            m_detector = detector;
            if (m_samplingCallback) {
                m_samplingCallback(interval, detector);
            }
            return formatSampling(interval, detector);
        }

        case 'B': {
            int forceMono = 0;
            if (!parseIntValue(arg, forceMono)) {
                return "";
            }
            bool mono = (forceMono != 0);
            m_forceMono = mono;
            if (m_forceMonoCallback) {
                m_forceMonoCallback(mono);
            }
            return std::string("B") + (mono ? "1" : "0");
        }

        case 'N':
            return "N" + std::to_string(m_pilotTenthsKHz.load());

        case 'o':
            return guestSession ? "o0,1" : "o1,0";

        default:
            return "";
    }
}

void XDRServer::setFrequencyCallback(FrequencyCallback cb) {
    std::lock_guard<std::mutex> lock(m_callbackMutex);
    m_freqCallback = cb;
}

void XDRServer::setVolumeCallback(VolumeCallback cb) {
    std::lock_guard<std::mutex> lock(m_callbackMutex);
    m_volCallback = cb;
}

void XDRServer::setGainCallback(GainCallback cb) {
    std::lock_guard<std::mutex> lock(m_callbackMutex);
    m_gainCallback = cb;
}

void XDRServer::setAGCCallback(AGCCallback cb) {
    std::lock_guard<std::mutex> lock(m_callbackMutex);
    m_agcCallback = cb;
}

void XDRServer::setModeCallback(IntCallback cb) {
    std::lock_guard<std::mutex> lock(m_callbackMutex);
    m_modeCallback = cb;
}

void XDRServer::setDeemphasisCallback(IntCallback cb) {
    std::lock_guard<std::mutex> lock(m_callbackMutex);
    m_deemphasisCallback = cb;
}

void XDRServer::setFilterCallback(IntCallback cb) {
    std::lock_guard<std::mutex> lock(m_callbackMutex);
    m_filterCallback = cb;
}

void XDRServer::setBandwidthCallback(IntCallback cb) {
    std::lock_guard<std::mutex> lock(m_callbackMutex);
    m_bandwidthCallback = cb;
}

void XDRServer::setAntennaCallback(IntCallback cb) {
    std::lock_guard<std::mutex> lock(m_callbackMutex);
    m_antennaCallback = cb;
}

void XDRServer::setSquelchCallback(IntCallback cb) {
    std::lock_guard<std::mutex> lock(m_callbackMutex);
    m_squelchCallback = cb;
}

void XDRServer::setRotatorCallback(IntCallback cb) {
    std::lock_guard<std::mutex> lock(m_callbackMutex);
    m_rotatorCallback = cb;
}

void XDRServer::setAlignmentCallback(IntCallback cb) {
    std::lock_guard<std::mutex> lock(m_callbackMutex);
    m_alignmentCallback = cb;
}

void XDRServer::setSamplingCallback(SamplingCallback cb) {
    std::lock_guard<std::mutex> lock(m_callbackMutex);
    m_samplingCallback = cb;
}

void XDRServer::setForceMonoCallback(ForceMonoCallback cb) {
    std::lock_guard<std::mutex> lock(m_callbackMutex);
    m_forceMonoCallback = cb;
}

void XDRServer::setStartCallback(StartCallback cb) {
    std::lock_guard<std::mutex> lock(m_callbackMutex);
    m_startCallback = cb;
}

void XDRServer::setStopCallback(StopCallback cb) {
    std::lock_guard<std::mutex> lock(m_callbackMutex);
    m_stopCallback = cb;
}

std::string XDRServer::processFmdxCommand(const std::string& cmd) {
    if (cmd.empty()) {
        return "";
    }

    char command = cmd[0];
    std::string arg = cmd.length() > 1 ? cmd.substr(1) : "";

    std::lock_guard<std::mutex> lock(m_callbackMutex);

    switch (command) {
        case 'T': {
            uint32_t freqHz = 0;
            if (!parseFrequencyHz(arg, freqHz)) {
                return "ER";
            }
            m_frequency = freqHz;
            if (m_freqCallback) {
                m_freqCallback(freqHz);
            }
            return "OK";
        }

        case 'Y': {
            int vol = 0;
            if (!parseIntValue(arg, vol)) {
                return "ER";
            }
            vol = std::clamp(vol, -100, 100);
            int mapped = std::clamp((vol + 100) / 2, 0, 100);
            m_volume = mapped;
            if (m_volCallback) {
                m_volCallback(mapped);
            }
            return "OK";
        }

        case 'A': {
            int agc = 0;
            if (!parseIntValue(arg, agc)) {
                return "ER";
            }
            agc = std::clamp(agc, 0, 2);
            bool accepted = true;
            if (m_agcCallback) {
                accepted = m_agcCallback(agc);
            }
            if (!accepted) {
                return "ER";
            }
            m_agcMode = agc;
            return "OK";
        }

        case 'I': {
            std::ostringstream oss;
            oss << "F=" << m_frequency;
            return oss.str();
        }

        default:
            return "ER";
    }
}
