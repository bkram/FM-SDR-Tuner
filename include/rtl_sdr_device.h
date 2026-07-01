#ifndef RTL_SDR_DEVICE_H
#define RTL_SDR_DEVICE_H

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

class RTLSDRDevice {
public:
  explicit RTLSDRDevice(uint32_t deviceIndex);
  ~RTLSDRDevice();

  bool connect();
  void disconnect();

  bool setFrequency(uint32_t freqHz);
  bool setSampleRate(uint32_t rate);
  bool setFrequencyCorrection(int ppm);
  bool setGainMode(bool manual);
  bool setGain(uint32_t gainTenthsDb);
  bool setAGC(bool enable);
  void setLowLatencyMode(bool enable);
  size_t readIQ(uint8_t *buffer, size_t maxSamples);
  // Drop all currently buffered IQ so the next readIQ returns only samples
  // captured after this call. Used after a scan retune to discard the stale
  // pre-retune ring contents (which otherwise mis-bin the FFT by one sweep
  // step). Cheap: just advances the read cursor to the write cursor.
  void flushBuffers();

private:
  static void asyncCallback(unsigned char *buf, uint32_t len, void *ctx);
  void asyncReadLoop();
  size_t availableBytesLocked() const;

  uint32_t m_deviceIndex;
  std::atomic<bool> m_connected;
  void *m_deviceHandle;
  std::vector<int> m_supportedGains;
  std::thread m_asyncThread;
  std::atomic<bool> m_asyncRunning;
  std::atomic<bool> m_asyncFailed;
  std::mutex m_bufferMutex;
  std::condition_variable m_bufferCv;
  std::vector<uint8_t> m_iqRing;
  size_t m_ringReadPos;
  size_t m_ringWritePos;
  bool m_ringFull;
  std::atomic<bool> m_lowLatencyMode;
  std::atomic<uint32_t> m_lowLatencyDropEvents;
  std::atomic<uint32_t> m_lowLatencyDeadlineEvents;
  std::atomic<uint32_t> m_lowLatencyShortReads;
};

#endif
