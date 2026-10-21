#ifndef REST_SERVER_H
#define REST_SERVER_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
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
  // Joins the client threads that have already finished. Called from the
  // accept loop so finished connections do not accumulate for the lifetime
  // of the process.
  void reapFinishedClientThreads();
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

  // Connections are served concurrently, so calls into the control plane are
  // serialized here. The controls touch shared tuner state and the panel
  // snapshot; before connections were served inline on a single thread that
  // serialization was implicit. Recursive because applyParams() calls
  // statusJson() to embed the current state in its reply.
  std::recursive_mutex m_controlsMutex;

  // Each accepted connection is served on its own thread, so a client that
  // connects and then stalls cannot deny the API to every other client.
  std::mutex m_clientThreadsMutex;
  struct ClientThread {
    std::atomic<bool> done{false};
    std::thread thread;
  };
  std::vector<std::shared_ptr<ClientThread>> m_clientThreads;

  // The sockets of the connections currently being served. stop() shuts them
  // down so a worker blocked in recv() returns immediately instead of
  // waiting out the whole receive timeout.
  std::mutex m_clientSocketsMutex;
  std::vector<int> m_clientSockets;
};

#endif // REST_SERVER_H
