#ifndef XDR_SERVER_H
#define XDR_SERVER_H

#include <stdint.h>
#include <string>
#include <atomic>
#include <functional>
#include <memory>
#include <thread>
#include <mutex>

class XDRServer {
public:
    static constexpr uint16_t DEFAULT_PORT = 7373;
    static constexpr int SALT_LENGTH = 16;
    static constexpr int HASH_LENGTH = 40;

    using FrequencyCallback = std::function<void(uint32_t freqHz)>;
    using VolumeCallback = std::function<void(int volume)>;
    using GainCallback = std::function<void(int gain)>;
    using AGCCallback = std::function<void(bool enable)>;

    XDRServer(uint16_t port = DEFAULT_PORT);
    ~XDRServer();

    void setPassword(const std::string& password);
    void setGuestMode(bool enabled);

    bool start();
    void stop();

    void setFrequencyCallback(FrequencyCallback cb);
    void setVolumeCallback(VolumeCallback cb);
    void setGainCallback(GainCallback cb);
    void setAGCCallback(AGCCallback cb);

    uint32_t getFrequency() const { return m_frequency; }
    int getVolume() const { return m_volume; }
    int getGain() const { return m_gain; }
    bool getAGC() const { return m_agc; }

    bool isRunning() const { return m_running; }

private:
    void handleClient(int clientSocket);
    void handleFmdxClient(int clientSocket);
    void handleXdrClient(int clientSocket);
    std::string processCommand(const std::string& cmd);
    std::string processFmdxCommand(const std::string& cmd);
    std::string generateSalt();
    std::string computeSHA1(const std::string& salt, const std::string& password);
    bool authenticate(const std::string& salt, const std::string& passwordHash);

    uint16_t m_port;
    int m_serverSocket;
    std::atomic<bool> m_running;
    std::thread m_acceptThread;

    std::string m_password;
    bool m_guestMode;
    bool m_authenticated;

    std::atomic<uint32_t> m_frequency;
    std::atomic<int> m_volume;
    std::atomic<int> m_gain;
    std::atomic<bool> m_agc;

    FrequencyCallback m_freqCallback;
    VolumeCallback m_volCallback;
    GainCallback m_gainCallback;
    AGCCallback m_agcCallback;

    std::mutex m_callbackMutex;
};

#endif
