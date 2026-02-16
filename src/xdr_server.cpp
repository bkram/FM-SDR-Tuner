#include "xdr_server.h"
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <iostream>
#include <sstream>
#include <algorithm>
#include <cstdio>
#include <chrono>
#include <cerrno>
#include <iomanip>
#include <openssl/sha.h>
#include <openssl/rand.h>
#include <random>

namespace {
void setRecvTimeoutMs(int clientSocket, int timeoutMs) {
    struct timeval tv;
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    setsockopt(clientSocket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

bool recvLine(int clientSocket, std::string& line, size_t maxLen) {
    line.clear();

    while (line.length() < maxLen) {
        char ch = '\0';
        ssize_t n = recv(clientSocket, &ch, 1, 0);
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
}  // namespace

XDRServer::XDRServer(uint16_t port)
    : m_port(port)
    , m_serverSocket(-1)
    , m_running(false)
    , m_guestMode(false)
    , m_authenticated(false)
    , m_guestSession(false)
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
    , m_aci(-1) {
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

std::string XDRServer::generateSalt() {
    static const char chars[] = "QWERTYUIOPASDFGHJKLZXCVBNMqwertyuiopasdfghjklzxcvbnm0123456789_-";
    const int len = strlen(chars);
    unsigned char random_data[SALT_LENGTH];
    
    if (!RAND_bytes(random_data, sizeof(random_data))) {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dis(0, len - 1);
        std::string salt;
        for (int i = 0; i < SALT_LENGTH; i++) {
            salt += chars[dis(gen)];
        }
        return salt;
    }
    
    std::string salt;
    for (int i = 0; i < SALT_LENGTH; i++) {
        salt += chars[random_data[i] % len];
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
            strcasecmp(expected.c_str(), passwordHash.c_str()) == 0);
}

std::string XDRServer::buildLegacyStatus() const {
    std::ostringstream oss;
    oss << "F=" << m_frequency
        << " V=" << m_volume
        << " G=" << m_gain
        << " A=" << m_agcMode;
    return oss.str();
}

std::string XDRServer::buildXdrStateSnapshot() const {
    std::ostringstream oss;
    oss << "M" << m_mode << "\n"
        << "Y" << m_volume << "\n"
        << "T" << (m_frequency / 1000) << "\n"
        << "D" << m_deemphasis << "\n"
        << "A" << m_agcMode << "\n";

    if (m_bandwidth >= 0) {
        oss << "W" << m_bandwidth << "\n";
    } else {
        oss << "F" << m_filter << "\n";
    }

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

bool XDRServer::start() {
    if (m_running) {
        return false;
    }

    m_serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (m_serverSocket < 0) {
        std::cerr << "Failed to create server socket" << std::endl;
        return false;
    }

    int opt = 1;
    setsockopt(m_serverSocket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in serverAddr;
    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port = htons(m_port);

    if (bind(m_serverSocket, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) < 0) {
        std::cerr << "Failed to bind to port " << m_port << std::endl;
        close(m_serverSocket);
        m_serverSocket = -1;
        return false;
    }

    if (listen(m_serverSocket, 5) < 0) {
        std::cerr << "Failed to listen on port " << m_port << std::endl;
        close(m_serverSocket);
        m_serverSocket = -1;
        return false;
    }

    m_running = true;
    m_acceptThread = std::thread([this]() {
        while (m_running) {
            struct sockaddr_in clientAddr;
            socklen_t clientLen = sizeof(clientAddr);
            int clientSocket = accept(m_serverSocket, (struct sockaddr*)&clientAddr, &clientLen);

            if (clientSocket >= 0) {
                handleClient(clientSocket);
                close(clientSocket);
            }
        }
    });

    std::cout << "XDR server listening on port " << m_port << std::endl;
    return true;
}

void XDRServer::stop() {
    if (!m_running) {
        return;
    }

    m_running = false;

    if (m_serverSocket >= 0) {
        close(m_serverSocket);
        m_serverSocket = -1;
    }

    if (m_acceptThread.joinable()) {
        m_acceptThread.join();
    }
}

void XDRServer::handleClient(int clientSocket) {
    char clientIP[INET_ADDRSTRLEN];
    struct sockaddr_in clientAddr;
    socklen_t clientLen = sizeof(clientAddr);
    getpeername(clientSocket, (struct sockaddr*)&clientAddr, &clientLen);
    inet_ntop(AF_INET, &clientAddr.sin_addr, clientIP, INET_ADDRSTRLEN);
    
    std::cout << "Client connected from " << clientIP << std::endl;
    
    setRecvTimeoutMs(clientSocket, 200);
    char firstByte = '\0';
    ssize_t n = recv(clientSocket, &firstByte, 1, MSG_PEEK);
    setRecvTimeoutMs(clientSocket, 0);

    bool isFmdxProtocol = (n > 0 && firstByte == 'x');
    
    if (isFmdxProtocol) {
        // Consume the initial FM-DX handshake line ("x") before command mode.
        std::string handshake;
        if (!recvLine(clientSocket, handshake, 16) || handshake != "x") {
            std::cout << "Invalid FM-DX handshake payload" << std::endl;
            return;
        }
        std::cout << "Detected FM-DX protocol handshake" << std::endl;
        send(clientSocket, "1\n", 2, 0);
        handleFmdxClient(clientSocket);
    } else {
        std::cout << "Detected XDR protocol handshake" << std::endl;
        handleXdrClient(clientSocket, clientIP);
    }
}

void XDRServer::handleFmdxClient(int clientSocket) {
    bool tunerStarted = false;
    char cmdBuffer[256];
    std::string command;

    while (true) {
        ssize_t n = recv(clientSocket, cmdBuffer, sizeof(cmdBuffer) - 1, 0);
        if (n <= 0) {
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
                send(clientSocket, response.c_str(), response.length(), 0);
            } else {
                send(clientSocket, "ER\n", 3, 0);
            }
        }
    }

    if (tunerStarted && m_stopCallback) {
        m_stopCallback();
    }
}

void XDRServer::handleXdrClient(int clientSocket, const char* clientIP) {
    std::cout << "XDR client authenticating from " << clientIP << std::endl;
    m_authenticated = false;
    m_guestSession = false;
    
    std::string salt = generateSalt();
    
    std::cout << "Sending salt: '" << salt << "'" << std::endl;
    
    std::string msg = salt + "\n";
    send(clientSocket, msg.c_str(), msg.length(), 0);
    
    std::string authMsg;
    if (!recvLine(clientSocket, authMsg, HASH_LENGTH + 2)) {
        std::cout << "Client disconnected before sending auth hash" << std::endl;
        return;
    }

    std::string clientHash = authMsg;
    if (clientHash.length() == HASH_LENGTH + 1 &&
        (clientHash[0] == 'P' || clientHash[0] == 'p')) {
        clientHash = clientHash.substr(1);
    }
    
    std::cout << "Received auth payload: '" << authMsg << "' -> hash '" << clientHash
              << "' (length: " << clientHash.length() << ")" << std::endl;
    
    bool authSuccess = authenticate(salt, clientHash);
    std::cout << "Authentication " << (authSuccess ? "SUCCESS" : "FAILED") << std::endl;
    
    if (!authSuccess && !m_guestMode) {
        send(clientSocket, "a0\n", 3, 0);
        return;
    }
    
    if (!authSuccess && m_guestMode) {
        send(clientSocket, "a1\n", 3, 0);
        m_guestSession = true;
    } else {
        send(clientSocket, "a2\n", 3, 0);
        m_guestSession = false;
    }
    
    m_authenticated = authSuccess || m_guestMode;
    if (m_authenticated) {
        std::string online = m_guestSession ? "o0,1\n" : "o1,0\n";
        send(clientSocket, online.c_str(), online.length(), 0);

        std::string snapshot = buildXdrStateSnapshot();
        snapshot += "\n";
        send(clientSocket, snapshot.c_str(), snapshot.length(), 0);
    }
    
    char cmdBuffer[256];
    std::string command;
    setRecvTimeoutMs(clientSocket, 100);
    auto lastSignal = std::chrono::steady_clock::now();

    while (true) {
        ssize_t n = recv(clientSocket, cmdBuffer, sizeof(cmdBuffer) - 1, 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                int intervalMs = m_samplingInterval.load();
                if (intervalMs <= 0) {
                    intervalMs = 66;
                }
                auto now = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastSignal).count();
                if (elapsed >= intervalMs) {
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

            std::string response = processCommand(line);
            response += "\n";
            send(clientSocket, response.c_str(), response.length(), 0);
        }
    }
    setRecvTimeoutMs(clientSocket, 0);

    m_authenticated = false;
    m_guestSession = false;
}

std::string XDRServer::processCommand(const std::string& cmd) {
    if (cmd.empty()) {
        return "";
    }

    if (!m_authenticated) {
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
            return buildLegacyStatus();

        case 'T': {
            uint32_t freqHz = 0;
            if (!parseFrequencyHz(arg, freqHz)) {
                return "ERR";
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
                return "ERR";
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
                return "ERR";
            }
            daa = std::clamp(daa, -1000, 1000);
            m_daa = daa;
            if (m_alignmentCallback) {
                m_alignmentCallback(daa);
            }
            return "V" + std::to_string(daa);
        }

        case 'G': {
            int gain = 0;
            if (!parseIntValue(arg, gain)) {
                return "ERR";
            }
            gain = std::clamp(gain, 0, 99);
            m_gain = gain;
            if (m_gainCallback) {
                m_gainCallback(gain);
            }
            return formatGain(gain);
        }

        case 'A': {
            int agcMode = 0;
            if (!parseIntValue(arg, agcMode)) {
                return "ERR";
            }
            agcMode = std::clamp(agcMode, 0, 2);
            m_agcMode = agcMode;
            if (m_agcCallback) {
                m_agcCallback(agcMode);
            }
            return "A" + std::to_string(agcMode);
        }

        case 'M': {
            int mode = 0;
            if (!parseIntValue(arg, mode)) {
                return "ERR";
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
                return "ERR";
            }
            deemph = std::clamp(deemph, 0, 1);
            m_deemphasis = deemph;
            if (m_deemphasisCallback) {
                m_deemphasisCallback(deemph);
            }
            return "D" + std::to_string(deemph);
        }

        case 'F': {
            int filter = 0;
            if (!parseIntValue(arg, filter)) {
                return "ERR";
            }
            m_filter = filter;
            if (m_filterCallback) {
                m_filterCallback(filter);
            }
            return "F" + std::to_string(filter);
        }

        case 'W': {
            int bandwidth = 0;
            if (!parseIntValue(arg, bandwidth)) {
                return "ERR";
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
                return "ERR";
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
                return "ERR";
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
                return "ERR";
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
                    return "ERR";
                }
            } else {
                if (!parseIntValue(arg.substr(0, comma), interval) ||
                    !parseIntValue(arg.substr(comma + 1), detector)) {
                    return "ERR";
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
                return "ERR";
            }
            bool mono = (forceMono != 0);
            m_forceMono = mono;
            if (m_forceMonoCallback) {
                m_forceMonoCallback(mono);
            }
            return std::string("B") + (mono ? "1" : "0");
        }

        case 'N':
            return "N0";

        case 'o':
            return m_guestSession ? "o0,1" : "o1,0";

        default:
            return "ERR";
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
            m_agcMode = agc;
            if (m_agcCallback) {
                m_agcCallback(agc);
            }
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
