#ifndef SDRPLAY_DEVICE_H
#define SDRPLAY_DEVICE_H

#include <atomic>
#include <complex>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

// SDRplay RSP backend. The SDRplay API (sdrplay_api) is a proprietary library
// the user installs separately; we dlopen it at runtime (no build-time link, no
// bundling) so the app stays GPL-clean and falls back to RTL-SDR when SDRplay
// is absent. The RSP streams 16-bit IQ via the API callback into a ring; the
// demod drains it as normalized complex<float> (full ADC dynamic range), while
// the legacy uint8 readIQ() quantizes the same samples back to RTL's 8-bit,
// full-scale-referenced format so the signal meter / scan engine are reused
// unchanged.
//
// When the SDRplay SDK headers are not present at build time
// (FM_TUNER_HAS_SDRPLAY undefined), this compiles to a stub that reports
// "unavailable" so the rest of the tuner builds anywhere.
class SDRplayDevice {
public:
  explicit SDRplayDevice(uint32_t deviceIndex = 0);
  ~SDRplayDevice();

  /// True if the SDRplay API library can be loaded (built with the SDK and the
  /// runtime library present).
  static bool apiAvailable();
  /// Number of attached RSP devices (0 if API unavailable).
  static int deviceCount();

  bool connect();
  void disconnect();

  bool setFrequency(uint32_t freqHz);
  bool setSampleRate(uint32_t rate);     // RSP rate is fixed; validates the request
  bool setFrequencyCorrection(int ppm);
  bool setGainMode(bool manual);         // manual disables AGC
  bool setGain(uint32_t gainTenthsDb);   // tenths of dB -> RSP IF gain reduction
  bool setAGC(bool enable);
  bool setBandwidthHz(int hz);           // IF channel bandwidth (maps to RSP bw steps)
  bool setLnaState(int state);           // front-end LNA gain-reduction step (0 = most gain)
  // Scan-only wide mode: drop the hardware decimation (256 kHz -> 2.048 MHz)
  // and widen the IF filter so a spectral scan covers ~8x more spectrum per
  // retune. Live via sdrplay_api Update (no re-init). Toggle off to restore the
  // 256 kHz audio rate. Flushes the ring on switch.
  bool setScanWideMode(bool wide);
  bool setAntenna(int index);            // RSP antenna input (model-specific)
  bool setBiasTee(bool enable);          // RSP bias tee (model-specific)

  int antennaCount() const;
  int hwVer() const { return m_hwVer; }
  const char *modelName() const;

  /// Effective IQ sample rate delivered to the demod (after RSP decimation).
  int inputRate() const { return m_inputRate; }
  /// True after a fatal streaming error / device loss.
  bool failed() const { return m_failed.load(std::memory_order_relaxed); }

  /// Drain up to maxSamples complex IQ samples (normalized to ±1.0). Full
  /// 16-bit dynamic range — the demod path uses this.
  size_t readIQ(std::complex<float> *out, size_t maxSamples);
  /// Drain up to maxSamples IQ samples as interleaved uint8 (RTL format, same
  /// full-scale reference). Used by the signal meter / scan engine.
  size_t readIQ(uint8_t *out, size_t maxSamples);

  // Called from the SDRplay stream callback (file-local in the .cpp).
  void ingest(const short *xi, const short *xq, unsigned int n, bool reset = false);
  void markFailed() { m_failed.store(true, std::memory_order_relaxed); }

private:
  size_t drainLocked(std::complex<float> *out, size_t maxSamples);

  int m_inputRate = 256000;
  int m_hwVer = 0;
  std::atomic<bool> m_connected{false};
  std::atomic<bool> m_failed{false};

  std::mutex m_mutex;
  std::condition_variable m_cv;
  std::vector<std::complex<float>> m_ring;
  size_t m_readPos = 0, m_writePos = 0;
  bool m_full = false;

  void *m_handle = nullptr;
};

#endif // SDRPLAY_DEVICE_H
