#ifndef TUNER_CONTROLLER_H
#define TUNER_CONTROLLER_H

#include <complex>
#include <cstddef>
#include <cstdint>
#include <string>

#include "rtl_sdr_device.h"
#include "rtl_tcp_client.h"
#include "sdrplay_device.h"

class TunerController {
public:
  enum class SourceKind { RtlSdr, RtlTcp, SdrPlay };
  // Native IQ sample format a source delivers. U8 = RTL's interleaved unsigned
  // 8-bit; CF32 = normalized complex<float> (SDRplay, full 16-bit range).
  enum class IqFormat { U8, CF32 };

  TunerController(const std::string &source, const std::string &tcpHost,
                  uint16_t tcpPort, uint32_t rtlDeviceIndex);

  SourceKind kind() const { return m_kind; }
  bool isDirectRtlSdr() const { return m_kind == SourceKind::RtlSdr; }
  bool isSdrPlay() const { return m_kind == SourceKind::SdrPlay; }
  IqFormat nativeFormat() const {
    return m_kind == SourceKind::SdrPlay ? IqFormat::CF32 : IqFormat::U8;
  }
  const char *name() const;

  // SDRplay-only knobs, applied at connect (and immediately when connected).
  // No-ops for RTL sources.
  void configureSdrplay(int lnaState, int antenna, bool biasTee);

  void setLowLatencyMode(bool enable);

  bool connect();
  void disconnect();
  bool setFrequency(uint32_t freqHz);
  bool setSampleRate(uint32_t sampleRate);
  bool setFrequencyCorrection(int ppm);
  bool setGainMode(bool manual);
  bool setGain(uint32_t gainTenthsDb);
  bool setAGC(bool enable);
  bool setBandwidthHz(int bandwidthHz);
  // Runtime SDRplay-only knobs (no-ops returning false on RTL sources).
  bool setLnaState(int state);
  bool setAntenna(int index);
  bool setBiasTee(bool enable);
  // Toggle the SDRplay wide-bandwidth scan mode. Returns the effective IQ
  // sample rate now in use (e.g. 2048000 when enabled, 256000 when disabled);
  // returns 0 for non-SDRplay sources (caller keeps its normal rate).
  uint32_t setScanWideMode(bool wide);
  // Drop buffered IQ so the next readIQ returns only post-call samples. Used
  // after a scan retune to discard stale pre-retune data. No-op where the
  // source has no large buffer (rtl_tcp reads on demand; SDRplay already
  // flushes its ring on retune).
  void flushBuffers();
  size_t readIQ(uint8_t *buffer, size_t maxSamples);
  size_t readIQ(std::complex<float> *buffer, size_t maxSamples);

  // Number of selectable antenna inputs (1 = none / not applicable).
  int antennaCount() const;
  // Human-readable device model ("RSPdx", "RSP1A", ...) for SDRplay; the source
  // name otherwise.
  const char *modelName() const;

  // True after a fatal streaming error / device loss (SDRplay only; always
  // false for RTL sources, which surface failures via zero-length reads).
  bool deviceFailed() const;
  // Effective IQ sample rate the source delivers (SDRplay reports its post
  // decimation rate; 0 = "use the configured iqSampleRate").
  int deliveredSampleRate() const;

private:
  SourceKind m_kind;
  RTLTCPClient m_rtlTcpClient;
  RTLSDRDevice m_rtlSdrDevice;
  SDRplayDevice m_sdrplayDevice;
  int m_sdrplayLnaState = 4;
  int m_sdrplayAntenna = 0;
  bool m_sdrplayBiasTee = false;
};

#endif
