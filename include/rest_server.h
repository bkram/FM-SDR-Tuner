#ifndef REST_SERVER_H
#define REST_SERVER_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

// Anonymous HTTP control API on a dedicated port, intended for an
// fm-dx-webserver client plugin. It exposes the SDR-specific settings the XDR
// protocol has no vocabulary for (manual dB gain, SDRplay LNA state, antenna
// input, bias-tee) alongside the common ones (frequency, bandwidth, AGC,
// de-emphasis, force-mono, volume, start/stop). There is NO authentication by
// design — bind it to localhost or a trusted network only.
//
// Control surface mirrors the MPXPrime meter's tuner settings and works for
// both the RTL-SDR and SDRplay backends (SDRplay-only knobs no-op on RTL).
class RestServer {
public:
  // Each setter returns whether it was applied/accepted. Any may be left null
  // (treated as "unsupported"). statusJson returns the current state as a JSON
  // object body.
  struct Controls {
    std::function<bool(uint32_t)> setFrequencyHz;
    std::function<bool(double)> setGainDb;       // manual gain in dB
    std::function<bool(bool)> setAutoGain;       // true = AGC / hardware auto gain
    std::function<bool(int)> setBandwidthHz;     // 0 = auto/widest
    std::function<bool(int)> setLnaState;        // SDRplay LNA step (0 = most gain)
    std::function<bool(int)> setAntenna;         // SDRplay antenna input index
    std::function<bool(bool)> setBiasTee;
    std::function<bool(int)> setPpm;
    std::function<bool(bool)> setRtlAgc;
    std::function<bool(int)> setDeemphasis;      // 0=50us,1=75us,2=off
    std::function<bool(int)> setBlendMode;       // 0=soft,1=normal,2=aggressive
    std::function<bool(bool)> setForceMono;
    std::function<bool(int)> setVolume;          // 0..100
    std::function<bool()> start;
    std::function<bool()> stop;
    std::function<bool()> resetStats; // restart MPX/RDS measurement windows
    std::function<std::string()> statusJson;
  };

  RestServer(std::string bindAddress, uint16_t port, Controls controls);
  ~RestServer();

  void setVerboseLogging(bool enabled) {
    m_verboseLogging.store(enabled, std::memory_order_relaxed);
  }

  bool start();
  void stop();
  bool isRunning() const { return m_running.load(std::memory_order_relaxed); }

private:
  void acceptLoop();
  void handleConnection(int clientSocket);
  // Applies the parsed key/value parameters and returns the JSON response body.
  std::string applyParams(const std::vector<std::pair<std::string, std::string>> &params,
                          int &appliedCount);

  std::string m_bindAddress;
  uint16_t m_port;
  Controls m_controls;
  int m_serverSocket = -1;
  std::atomic<bool> m_running{false};
  std::atomic<bool> m_verboseLogging{false};
  std::thread m_acceptThread;
};

#endif // REST_SERVER_H
