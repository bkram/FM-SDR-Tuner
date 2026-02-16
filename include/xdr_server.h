#ifndef XDR_SERVER_H
#define XDR_SERVER_H

#include <stdint.h>
#include <string>
#include <atomic>
#include <functional>
#include <memory>
#include <thread>
#include <mutex>
#include <chrono>

class XDRServer {
public:
    static constexpr uint16_t DEFAULT_PORT = 7373;
    static constexpr int SALT_LENGTH = 16;
    static constexpr int HASH_LENGTH = 40;

    using IntCallback = std::function<void(int value)>;
    using FrequencyCallback = std::function<void(uint32_t freqHz)>;
    using VolumeCallback = std::function<void(int volume)>;
    using GainCallback = std::function<void(int gain)>;
    using AGCCallback = std::function<void(int mode)>;
    using SamplingCallback = std::function<void(int interval, int detector)>;
    using ForceMonoCallback = std::function<void(bool forceMono)>;
    using StartCallback = std::function<void()>;
    using StopCallback = std::function<void()>;

    XDRServer(uint16_t port = DEFAULT_PORT);
    ~XDRServer();

    void setPassword(const std::string& password);
    void setGuestMode(bool enabled);

    bool start();
    void stop();
    void updateSignal(float level, bool stereo, bool forcedMono, int cci = -1, int aci = -1);

    void setFrequencyCallback(FrequencyCallback cb);
    void setVolumeCallback(VolumeCallback cb);
    void setGainCallback(GainCallback cb);
    void setAGCCallback(AGCCallback cb);
    void setModeCallback(IntCallback cb);
    void setDeemphasisCallback(IntCallback cb);
    void setFilterCallback(IntCallback cb);
    void setBandwidthCallback(IntCallback cb);
    void setAntennaCallback(IntCallback cb);
    void setSquelchCallback(IntCallback cb);
    void setRotatorCallback(IntCallback cb);
    void setAlignmentCallback(IntCallback cb);
    void setSamplingCallback(SamplingCallback cb);
    void setForceMonoCallback(ForceMonoCallback cb);
    void setStartCallback(StartCallback cb);
    void setStopCallback(StopCallback cb);

    uint32_t getFrequency() const { return m_frequency; }
    int getMode() const { return m_mode; }
    int getVolume() const { return m_volume; }
    int getGain() const { return m_gain; }
    int getAGCMode() const { return m_agcMode; }
    int getDeemphasis() const { return m_deemphasis; }
    int getFilter() const { return m_filter; }
    int getBandwidth() const { return m_bandwidth; }
    int getAntenna() const { return m_antenna; }
    int getAlignment() const { return m_daa; }
    int getSquelch() const { return m_squelch; }
    int getRotator() const { return m_rotator; }
    int getSamplingInterval() const { return m_samplingInterval; }
    int getDetector() const { return m_detector; }
    bool getForceMono() const { return m_forceMono; }

    bool isRunning() const { return m_running; }

private:
    void handleClient(int clientSocket);
    void handleFmdxClient(int clientSocket);
    void handleXdrClient(int clientSocket, const char* clientIP);
    std::string processCommand(const std::string& cmd);
    std::string processFmdxCommand(const std::string& cmd);
    std::string generateSalt();
    std::string computeSHA1(const std::string& salt, const std::string& password);
    bool authenticate(const std::string& salt, const std::string& passwordHash);
    std::string buildLegacyStatus() const;
    std::string buildXdrStateSnapshot() const;
    std::string buildSignalLine() const;

    uint16_t m_port;
    int m_serverSocket;
    std::atomic<bool> m_running;
    std::thread m_acceptThread;

    std::string m_password;
    bool m_guestMode;
    bool m_authenticated;
    bool m_guestSession;

    std::atomic<int> m_mode;
    std::atomic<uint32_t> m_frequency;
    std::atomic<int> m_volume;
    std::atomic<int> m_deemphasis;
    std::atomic<int> m_filter;
    std::atomic<int> m_bandwidth;
    std::atomic<int> m_antenna;
    std::atomic<int> m_gain;
    std::atomic<int> m_agcMode;
    std::atomic<int> m_daa;
    std::atomic<int> m_squelch;
    std::atomic<int> m_rotator;
    std::atomic<int> m_samplingInterval;
    std::atomic<int> m_detector;
    std::atomic<bool> m_forceMono;
    std::atomic<int> m_signalDeci;
    std::atomic<bool> m_signalStereo;
    std::atomic<bool> m_signalForcedMono;
    std::atomic<int> m_cci;
    std::atomic<int> m_aci;

    IntCallback m_modeCallback;
    FrequencyCallback m_freqCallback;
    VolumeCallback m_volCallback;
    IntCallback m_deemphasisCallback;
    IntCallback m_filterCallback;
    IntCallback m_bandwidthCallback;
    IntCallback m_antennaCallback;
    GainCallback m_gainCallback;
    AGCCallback m_agcCallback;
    IntCallback m_alignmentCallback;
    IntCallback m_squelchCallback;
    IntCallback m_rotatorCallback;
    SamplingCallback m_samplingCallback;
    ForceMonoCallback m_forceMonoCallback;
    StartCallback m_startCallback;
    StopCallback m_stopCallback;

    std::mutex m_callbackMutex;
};

#endif
