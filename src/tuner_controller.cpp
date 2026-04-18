#include "tuner_controller.h"

TunerController::TunerController(const std::string &source,
                                 const std::string &tcpHost, uint16_t tcpPort,
                                 uint32_t rtlDeviceIndex)
    : m_useDirectRtlSdr(source == "rtl_sdr"), m_rtlTcpClient(tcpHost, tcpPort),
      m_rtlSdrDevice(rtlDeviceIndex) {}

void TunerController::setLowLatencyMode(bool enable) {
  if (m_useDirectRtlSdr) {
    m_rtlSdrDevice.setLowLatencyMode(enable);
  }
}

bool TunerController::connect() {
  return m_useDirectRtlSdr ? m_rtlSdrDevice.connect() : m_rtlTcpClient.connect();
}

void TunerController::disconnect() {
  if (m_useDirectRtlSdr) {
    m_rtlSdrDevice.disconnect();
  } else {
    m_rtlTcpClient.disconnect();
  }
}

bool TunerController::setFrequency(uint32_t freqHz) {
  return m_useDirectRtlSdr ? m_rtlSdrDevice.setFrequency(freqHz)
                           : m_rtlTcpClient.setFrequency(freqHz);
}

bool TunerController::setSampleRate(uint32_t sampleRate) {
  return m_useDirectRtlSdr ? m_rtlSdrDevice.setSampleRate(sampleRate)
                           : m_rtlTcpClient.setSampleRate(sampleRate);
}

bool TunerController::setFrequencyCorrection(int ppm) {
  return m_useDirectRtlSdr ? m_rtlSdrDevice.setFrequencyCorrection(ppm)
                           : m_rtlTcpClient.setFrequencyCorrection(ppm);
}

bool TunerController::setGainMode(bool manual) {
  return m_useDirectRtlSdr ? m_rtlSdrDevice.setGainMode(manual)
                           : m_rtlTcpClient.setGainMode(manual);
}

bool TunerController::setGain(uint32_t gainTenthsDb) {
  return m_useDirectRtlSdr ? m_rtlSdrDevice.setGain(gainTenthsDb)
                           : m_rtlTcpClient.setGain(gainTenthsDb);
}

bool TunerController::setAGC(bool enable) {
  return m_useDirectRtlSdr ? m_rtlSdrDevice.setAGC(enable)
                           : m_rtlTcpClient.setAGC(enable);
}

size_t TunerController::readIQ(uint8_t *buffer, size_t maxSamples) {
  return m_useDirectRtlSdr ? m_rtlSdrDevice.readIQ(buffer, maxSamples)
                           : m_rtlTcpClient.readIQ(buffer, maxSamples);
}
