#include "xdr_server.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <iostream>
#include <sstream>
#include <openssl/sha.h>
#include <openssl/rand.h>
#include <random>

XDRServer::XDRServer(uint16_t port)
    : m_port(port)
    , m_serverSocket(-1)
    , m_running(false)
    , m_guestMode(false)
    , m_authenticated(false)
    , m_frequency(88500000)
    , m_volume(50)
    , m_gain(0)
    , m_agc(false) {
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
        sprintf(sha_string + (i * 2), "%02x", sha[i]);
    }
    return std::string(sha_string);
}

bool XDRServer::authenticate(const std::string& salt, const std::string& passwordHash) {
    std::string expected = computeSHA1(salt, m_password);
    return (expected.length() == passwordHash.length() && 
            strcasecmp(expected.c_str(), passwordHash.c_str()) == 0);
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
    std::string salt = generateSalt();
    
    std::string msg = salt + "\n";
    send(clientSocket, msg.c_str(), msg.length(), 0);
    
    char buffer[256];
    memset(buffer, 0, sizeof(buffer));
    ssize_t n = recv(clientSocket, buffer, HASH_LENGTH + 1, 0);
    
    std::string clientHash;
    if (n > 0) {
        clientHash = std::string(buffer, n);
        if (!clientHash.empty() && clientHash.back() == '\n') {
            clientHash.pop_back();
        }
        if (!clientHash.empty() && clientHash.back() == '\r') {
            clientHash.pop_back();
        }
    }
    
    bool authSuccess = authenticate(salt, clientHash);
    
    if (!authSuccess && !m_guestMode) {
        send(clientSocket, "a0\n", 3, 0);
        return;
    }
    
    if (!authSuccess && m_guestMode) {
        send(clientSocket, "a1\n", 3, 0);
    } else {
        send(clientSocket, "a2\n", 3, 0);
    }
    
    m_authenticated = authSuccess || m_guestMode;
    
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

            std::string response = processCommand(line);
            response += "\n";
            send(clientSocket, response.c_str(), response.length(), 0);
        }
    }
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
        case 'P':
            return "a2";

        case 'S': {
            std::ostringstream oss;
            oss << "F=" << m_frequency
                << " V=" << m_volume
                << " G=" << m_gain
                << " A=" << (m_agc ? 1 : 0);
            return oss.str();
        }

        case 'T': {
            if (arg.empty()) {
                return "ERR";
            }
            try {
                uint32_t freq = std::stoul(arg);
                m_frequency = freq;
                if (m_freqCallback) {
                    m_freqCallback(freq);
                }
                return "OK";
            } catch (...) {
                return "ERR";
            }
        }

        case 'V': {
            if (arg.empty()) {
                return "ERR";
            }
            try {
                int vol = std::stoi(arg);
                if (vol < 0) vol = 0;
                if (vol > 100) vol = 100;
                m_volume = vol;
                if (m_volCallback) {
                    m_volCallback(vol);
                }
                return "OK";
            } catch (...) {
                return "ERR";
            }
        }

        case 'G': {
            if (arg.empty()) {
                return "ERR";
            }
            try {
                int gain = std::stoi(arg);
                m_gain = gain;
                if (m_gainCallback) {
                    m_gainCallback(gain);
                }
                return "OK";
            } catch (...) {
                return "ERR";
            }
        }

        case 'A': {
            if (arg.empty()) {
                return "ERR";
            }
            try {
                int agc = std::stoi(arg);
                m_agc = (agc != 0);
                if (m_agcCallback) {
                    m_agcCallback(m_agc);
                }
                return "OK";
            } catch (...) {
                return "ERR";
            }
        }

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
