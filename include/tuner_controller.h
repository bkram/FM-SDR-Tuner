#ifndef TUNER_CONTROLLER_H
#define TUNER_CONTROLLER_H

#include <cstddef>
#include <cstdint>
#include <string>

#include "rtl_sdr_device.h"
#include "rtl_tcp_client.h"

class TunerController {
public:
  TunerController(const std::string &source, const std::string &tcpHost,
                  uint16_t tcpPort, uint32_t rtlDeviceIndex);

  bool isDirectRtlSdr() const { return m_useDirectRtlSdr; }
  const char *name() const { return m_useDirectRtlSdr ? "rtl_sdr" : "rtl_tcp"; }

  void setLowLatencyMode(bool enable);

  bool connect();
  void disconnect();
  bool setFrequency(uint32_t freqHz);
  bool setSampleRate(uint32_t sampleRate);
  bool setFrequencyCorrection(int ppm);
  bool setGainMode(bool manual);
  bool setGain(uint32_t gainTenthsDb);
  bool setAGC(bool enable);
  size_t readIQ(uint8_t *buffer, size_t maxSamples);

private:
  bool m_useDirectRtlSdr;
  RTLTCPClient m_rtlTcpClient;
  RTLSDRDevice m_rtlSdrDevice;
};

#endif
