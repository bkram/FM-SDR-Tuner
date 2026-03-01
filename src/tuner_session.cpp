#include "tuner_session.h"

#include <iostream>

TunerSession::TunerSession(
    TunerController &tuner, bool &rtlConnected, Params params,
    std::function<uint32_t()> requestedFrequencyHz,
    std::function<int()> requestedAGCMode,
    std::function<int()> requestedCustomGain,
    std::function<void(const char *reason)> applyRtlGainAndAgc)
    : m_tuner(tuner), m_rtlConnected(rtlConnected), m_params(std::move(params)),
      m_requestedFrequencyHz(std::move(requestedFrequencyHz)),
      m_requestedAGCMode(std::move(requestedAGCMode)),
      m_requestedCustomGain(std::move(requestedCustomGain)),
      m_applyRtlGainAndAgc(std::move(applyRtlGainAndAgc)) {}

const char *TunerSession::tunerName() const { return m_tuner.name(); }

void TunerSession::connect() {
  if (m_rtlConnected) {
    return;
  }
  if (m_params.useDirectRtlSdr) {
    std::cout << "[SDR] connecting to rtl_sdr device " << m_params.rtlDeviceIndex
              << "...\n";
  } else {
    std::cout << "[SDR] connecting to rtl_tcp at " << m_params.tcpHost << ":"
              << m_params.tcpPort << "...\n";
  }
  if (!m_tuner.connect()) {
    std::cerr << "[SDR] warning: failed to connect to " << tunerName() << "\n";
    return;
  }

  std::cout << "[SDR] connected; setting frequency to " << m_params.initialFreqKHz
            << " kHz...\n";
  const bool okFreq = m_tuner.setFrequency(m_requestedFrequencyHz());
  const bool okRate = m_tuner.setSampleRate(m_params.iqSampleRate);
  bool okPpm = true;
  if (m_params.freqCorrectionPpm != 0) {
    okPpm = m_tuner.setFrequencyCorrection(m_params.freqCorrectionPpm);
  }
  if (!okFreq || !okRate) {
    std::cerr << "[SDR] warning: failed to initialize " << tunerName()
              << " stream (setFrequency=" << (okFreq ? 1 : 0)
              << ", setSampleRate=" << (okRate ? 1 : 0)
              << ", setPpm=" << (okPpm ? 1 : 0) << ")\n";
    m_tuner.disconnect();
    return;
  }
  if (!okPpm) {
    std::cerr << "[SDR] warning: failed to apply frequency correction ppm="
              << m_params.freqCorrectionPpm
              << " (continuing without ppm correction)\n";
  }
  if (m_params.verboseLogging) {
    std::cout << "[SDR] applying TEF AGC mode " << m_requestedAGCMode()
              << " and custom gain flags G" << m_requestedCustomGain()
              << "...\n";
  }
  m_rtlConnected = true;
  m_applyRtlGainAndAgc("connect/apply");
}

void TunerSession::disconnect() {
  if (!m_rtlConnected) {
    return;
  }
  m_tuner.disconnect();
  m_rtlConnected = false;
  std::cout << "[SDR] disconnected from " << tunerName() << "\n";
}

void TunerSession::resetReadFailures() { m_consecutiveReadFailures = 0; }

void TunerSession::noteReadFailureAndMaybeReconnect() {
  m_consecutiveReadFailures++;
  if (m_params.autoReconnect && m_rtlConnected && m_consecutiveReadFailures >= 20) {
    std::cerr << "[SDR] no IQ data, reconnecting...\n";
    disconnect();
    connect();
    m_consecutiveReadFailures = 0;
  }
}
