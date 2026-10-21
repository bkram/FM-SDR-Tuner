#ifndef RTL_TCP_CLIENT_H
#define RTL_TCP_CLIENT_H

#include <atomic>
#include <memory>
#include <stddef.h>
#include <stdint.h>
#include <string>

class RTLTCPClient {
public:
  RTLTCPClient(const std::string &host, uint16_t port);
  ~RTLTCPClient();

  bool connect();
  void disconnect();

  bool isConnected() const { return m_connected; }

  size_t readIQ(uint8_t *buffer, size_t maxSamples);

  // Drop the IQ that is still in flight from the previous centre frequency.
  // A USB dongle can have its ring emptied; a network source cannot, so the
  // stale bytes sitting in the socket buffer are drained instead. Without
  // this a retune measures the spectrum of the band that was just left.
  void flushBuffers();

  bool setFrequency(uint32_t freqHz);
  bool setSampleRate(uint32_t rate);
  bool setFrequencyCorrection(int ppm);
  bool setGainMode(bool manual);
  bool setGain(uint32_t gain);
  bool setAGC(bool enable);

  uint32_t getFrequency() const { return m_frequency; }
  uint32_t getSampleRate() const { return m_sampleRate; }

private:
  bool sendCommand(uint8_t cmd, uint32_t param);
  bool sendAll(const uint8_t *data, size_t len);
  bool readResponse(uint8_t *buffer, size_t len);

  std::string m_host;
  uint16_t m_port;
  int m_socket;
  std::atomic<bool> m_connected;
  std::atomic<uint32_t> m_frequency;
  std::atomic<uint32_t> m_sampleRate;
  bool m_havePendingIqByte;
  uint8_t m_pendingIqByte;
};

#endif
