#include "tuner_controller.h"

namespace {
TunerController::SourceKind kindFromSource(const std::string &source) {
  if (source == "sdrplay") {
    return TunerController::SourceKind::SdrPlay;
  }
  if (source == "rtl_tcp") {
    return TunerController::SourceKind::RtlTcp;
  }
  return TunerController::SourceKind::RtlSdr;
}
} // namespace

TunerController::TunerController(const std::string &source,
                                 const std::string &tcpHost, uint16_t tcpPort,
                                 uint32_t rtlDeviceIndex)
    : m_kind(kindFromSource(source)), m_rtlTcpClient(tcpHost, tcpPort),
      m_rtlSdrDevice(rtlDeviceIndex), m_sdrplayDevice(rtlDeviceIndex) {}

const char *TunerController::name() const {
  switch (m_kind) {
  case SourceKind::SdrPlay:
    return "sdrplay";
  case SourceKind::RtlTcp:
    return "rtl_tcp";
  case SourceKind::RtlSdr:
  default:
    return "rtl_sdr";
  }
}

void TunerController::configureSdrplay(int lnaState, int antenna, bool biasTee) {
  m_sdrplayLnaState = lnaState;
  m_sdrplayAntenna = antenna;
  m_sdrplayBiasTee = biasTee;
}

void TunerController::setLowLatencyMode(bool enable) {
  if (m_kind == SourceKind::RtlSdr) {
    m_rtlSdrDevice.setLowLatencyMode(enable);
  }
}

bool TunerController::connect() {
  switch (m_kind) {
  case SourceKind::SdrPlay: {
    if (!m_sdrplayDevice.connect()) {
      return false;
    }
    m_sdrplayDevice.setLnaState(m_sdrplayLnaState);
    if (m_sdrplayAntenna > 0) {
      m_sdrplayDevice.setAntenna(m_sdrplayAntenna);
    }
    if (m_sdrplayBiasTee) {
      m_sdrplayDevice.setBiasTee(true);
    }
    return true;
  }
  case SourceKind::RtlTcp:
    return m_rtlTcpClient.connect();
  case SourceKind::RtlSdr:
  default:
    return m_rtlSdrDevice.connect();
  }
}

void TunerController::disconnect() {
  switch (m_kind) {
  case SourceKind::SdrPlay:
    m_sdrplayDevice.disconnect();
    break;
  case SourceKind::RtlTcp:
    m_rtlTcpClient.disconnect();
    break;
  case SourceKind::RtlSdr:
  default:
    m_rtlSdrDevice.disconnect();
    break;
  }
}

bool TunerController::setFrequency(uint32_t freqHz) {
  switch (m_kind) {
  case SourceKind::SdrPlay:
    return m_sdrplayDevice.setFrequency(freqHz);
  case SourceKind::RtlTcp:
    return m_rtlTcpClient.setFrequency(freqHz);
  case SourceKind::RtlSdr:
  default:
    return m_rtlSdrDevice.setFrequency(freqHz);
  }
}

bool TunerController::setSampleRate(uint32_t sampleRate) {
  switch (m_kind) {
  case SourceKind::SdrPlay:
    return m_sdrplayDevice.setSampleRate(sampleRate);
  case SourceKind::RtlTcp:
    return m_rtlTcpClient.setSampleRate(sampleRate);
  case SourceKind::RtlSdr:
  default:
    return m_rtlSdrDevice.setSampleRate(sampleRate);
  }
}

bool TunerController::setFrequencyCorrection(int ppm) {
  switch (m_kind) {
  case SourceKind::SdrPlay:
    return m_sdrplayDevice.setFrequencyCorrection(ppm);
  case SourceKind::RtlTcp:
    return m_rtlTcpClient.setFrequencyCorrection(ppm);
  case SourceKind::RtlSdr:
  default:
    return m_rtlSdrDevice.setFrequencyCorrection(ppm);
  }
}

bool TunerController::setGainMode(bool manual) {
  switch (m_kind) {
  case SourceKind::SdrPlay:
    return m_sdrplayDevice.setGainMode(manual);
  case SourceKind::RtlTcp:
    return m_rtlTcpClient.setGainMode(manual);
  case SourceKind::RtlSdr:
  default:
    return m_rtlSdrDevice.setGainMode(manual);
  }
}

bool TunerController::setGain(uint32_t gainTenthsDb) {
  switch (m_kind) {
  case SourceKind::SdrPlay:
    return m_sdrplayDevice.setGain(gainTenthsDb);
  case SourceKind::RtlTcp:
    return m_rtlTcpClient.setGain(gainTenthsDb);
  case SourceKind::RtlSdr:
  default:
    return m_rtlSdrDevice.setGain(gainTenthsDb);
  }
}

bool TunerController::setAGC(bool enable) {
  switch (m_kind) {
  case SourceKind::SdrPlay:
    return m_sdrplayDevice.setAGC(enable);
  case SourceKind::RtlTcp:
    return m_rtlTcpClient.setAGC(enable);
  case SourceKind::RtlSdr:
  default:
    return m_rtlSdrDevice.setAGC(enable);
  }
}

bool TunerController::setBandwidthHz(int bandwidthHz) {
  if (m_kind == SourceKind::SdrPlay) {
    return m_sdrplayDevice.setBandwidthHz(bandwidthHz);
  }
  return true; // RTL channel bandwidth is handled in the DSP pipeline
}

bool TunerController::setLnaState(int state) {
  m_sdrplayLnaState = state;
  return m_kind == SourceKind::SdrPlay && m_sdrplayDevice.setLnaState(state);
}

bool TunerController::setAntenna(int index) {
  m_sdrplayAntenna = index;
  return m_kind == SourceKind::SdrPlay && m_sdrplayDevice.setAntenna(index);
}

bool TunerController::setBiasTee(bool enable) {
  m_sdrplayBiasTee = enable;
  return m_kind == SourceKind::SdrPlay && m_sdrplayDevice.setBiasTee(enable);
}

uint32_t TunerController::setScanWideMode(bool wide) {
  if (m_kind != SourceKind::SdrPlay) {
    return 0; // RTL has no wide mode; caller keeps its normal rate
  }
  m_sdrplayDevice.setScanWideMode(wide);
  return static_cast<uint32_t>(m_sdrplayDevice.inputRate());
}

void TunerController::flushBuffers() {
  if (m_kind == SourceKind::RtlSdr) {
    m_rtlSdrDevice.flushBuffers();
  }
}

size_t TunerController::readIQ(uint8_t *buffer, size_t maxSamples) {
  switch (m_kind) {
  case SourceKind::SdrPlay:
    return m_sdrplayDevice.readIQ(buffer, maxSamples);
  case SourceKind::RtlTcp:
    return m_rtlTcpClient.readIQ(buffer, maxSamples);
  case SourceKind::RtlSdr:
  default:
    return m_rtlSdrDevice.readIQ(buffer, maxSamples);
  }
}

size_t TunerController::readIQ(std::complex<float> *buffer, size_t maxSamples) {
  if (m_kind == SourceKind::SdrPlay) {
    return m_sdrplayDevice.readIQ(buffer, maxSamples);
  }
  return 0;
}

int TunerController::antennaCount() const {
  return m_kind == SourceKind::SdrPlay ? m_sdrplayDevice.antennaCount() : 1;
}

const char *TunerController::modelName() const {
  return m_kind == SourceKind::SdrPlay ? m_sdrplayDevice.modelName() : name();
}

bool TunerController::deviceFailed() const {
  return m_kind == SourceKind::SdrPlay && m_sdrplayDevice.failed();
}

int TunerController::deliveredSampleRate() const {
  if (m_kind == SourceKind::SdrPlay) {
    return m_sdrplayDevice.inputRate();
  }
  return 0;
}
