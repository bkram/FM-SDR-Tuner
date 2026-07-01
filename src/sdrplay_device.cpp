#include "sdrplay_device.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

#if defined(FM_TUNER_HAS_SDRPLAY)

#include <dlfcn.h>
#include <sdrplay_api.h>

namespace {

// Dynamically-loaded SDRplay API entry points (the library is proprietary and
// installed by the user; we never link or bundle it).
struct SDRplayApi {
  void *lib = nullptr;
  sdrplay_api_Open_t Open = nullptr;
  sdrplay_api_Close_t Close = nullptr;
  sdrplay_api_ApiVersion_t ApiVersion = nullptr;
  sdrplay_api_LockDeviceApi_t Lock = nullptr;
  sdrplay_api_UnlockDeviceApi_t Unlock = nullptr;
  sdrplay_api_GetDevices_t GetDevices = nullptr;
  sdrplay_api_SelectDevice_t SelectDevice = nullptr;
  sdrplay_api_ReleaseDevice_t ReleaseDevice = nullptr;
  sdrplay_api_GetDeviceParams_t GetDeviceParams = nullptr;
  sdrplay_api_Init_t Init = nullptr;
  sdrplay_api_Uninit_t Uninit = nullptr;
  sdrplay_api_Update_t Update = nullptr;

  bool ok() const {
    return lib && Open && Close && Lock && Unlock && GetDevices && SelectDevice &&
           ReleaseDevice && GetDeviceParams && Init && Uninit && Update;
  }
};

SDRplayApi &api() {
  static SDRplayApi a;
  static bool tried = false;
  if (!tried) {
    tried = true;
    const char *names[] = {"libsdrplay_api.dylib",
                           "/usr/local/lib/libsdrplay_api.dylib",
                           "libsdrplay_api.so", "libsdrplay_api.so.3",
                           "/usr/local/lib/libsdrplay_api.so"};
    for (const char *n : names) {
      a.lib = dlopen(n, RTLD_NOW | RTLD_GLOBAL);
      if (a.lib) break;
    }
    if (a.lib) {
      auto sym = [&](const char *n) { return dlsym(a.lib, n); };
      a.Open = reinterpret_cast<sdrplay_api_Open_t>(sym("sdrplay_api_Open"));
      a.Close = reinterpret_cast<sdrplay_api_Close_t>(sym("sdrplay_api_Close"));
      a.ApiVersion = reinterpret_cast<sdrplay_api_ApiVersion_t>(sym("sdrplay_api_ApiVersion"));
      a.Lock = reinterpret_cast<sdrplay_api_LockDeviceApi_t>(sym("sdrplay_api_LockDeviceApi"));
      a.Unlock = reinterpret_cast<sdrplay_api_UnlockDeviceApi_t>(sym("sdrplay_api_UnlockDeviceApi"));
      a.GetDevices = reinterpret_cast<sdrplay_api_GetDevices_t>(sym("sdrplay_api_GetDevices"));
      a.SelectDevice = reinterpret_cast<sdrplay_api_SelectDevice_t>(sym("sdrplay_api_SelectDevice"));
      a.ReleaseDevice = reinterpret_cast<sdrplay_api_ReleaseDevice_t>(sym("sdrplay_api_ReleaseDevice"));
      a.GetDeviceParams = reinterpret_cast<sdrplay_api_GetDeviceParams_t>(sym("sdrplay_api_GetDeviceParams"));
      a.Init = reinterpret_cast<sdrplay_api_Init_t>(sym("sdrplay_api_Init"));
      a.Uninit = reinterpret_cast<sdrplay_api_Uninit_t>(sym("sdrplay_api_Uninit"));
      a.Update = reinterpret_cast<sdrplay_api_Update_t>(sym("sdrplay_api_Update"));
    }
  }
  return a;
}

// Single active device (one RSP at a time).
sdrplay_api_DeviceT g_device;
sdrplay_api_DeviceParamsT *g_params = nullptr;

constexpr double kFsHz = 2048000.0;
constexpr int kDecim = 8; // 2.048 MHz / 8 = 256000, matching the tuner's INPUT_RATE
constexpr uint32_t kDefaultFreqHz = 98000000;

void streamA(short *xi, short *xq, sdrplay_api_StreamCbParamsT *p,
             unsigned int n, unsigned int reset, void *ctx) {
  (void)p;
  if (ctx) static_cast<SDRplayDevice *>(ctx)->ingest(xi, xq, n, reset != 0);
}

void eventCb(sdrplay_api_EventT id, sdrplay_api_TunerSelectT t,
             sdrplay_api_EventParamsT *p, void *ctx) {
  (void)t;
  (void)p;
  if (ctx && (id == sdrplay_api_DeviceRemoved || id == sdrplay_api_DeviceFailure)) {
    static_cast<SDRplayDevice *>(ctx)->markFailed();
  }
}

sdrplay_api_Bw_MHzT bwForHz(int hz) {
  if (hz <= 0) return sdrplay_api_BW_0_600; // auto -> wide enough for full MPX
  if (hz <= 250000) return sdrplay_api_BW_0_200;
  if (hz <= 450000) return sdrplay_api_BW_0_300;
  if (hz <= 1000000) return sdrplay_api_BW_0_600;
  return sdrplay_api_BW_1_536;
}

} // namespace

SDRplayDevice::SDRplayDevice(uint32_t) { m_ring.resize(1 << 18); }
SDRplayDevice::~SDRplayDevice() { disconnect(); }

bool SDRplayDevice::apiAvailable() {
  auto &a = api();
  if (!a.ok()) return false;
  if (a.Open() != sdrplay_api_Success) return false;
  a.Close();
  return true;
}

int SDRplayDevice::deviceCount() {
  auto &a = api();
  if (!a.ok()) return 0;
  if (a.Open() != sdrplay_api_Success) return 0;
  unsigned int n = 0;
  sdrplay_api_DeviceT devs[8];
  a.Lock();
  a.GetDevices(devs, &n, 8);
  a.Unlock();
  a.Close();
  return static_cast<int>(n);
}

bool SDRplayDevice::connect() {
  auto &a = api();
  if (!a.ok()) return false;
  if (a.Open() != sdrplay_api_Success) return false;

  unsigned int n = 0;
  sdrplay_api_DeviceT devs[8];
  a.Lock();
  if (a.GetDevices(devs, &n, 8) != sdrplay_api_Success || n == 0) {
    a.Unlock();
    a.Close();
    return false;
  }
  g_device = devs[0];
  if (a.SelectDevice(&g_device) != sdrplay_api_Success) {
    a.Unlock();
    a.Close();
    return false;
  }
  a.Unlock();

  if (a.GetDeviceParams(g_device.dev, &g_params) != sdrplay_api_Success || !g_params) {
    a.ReleaseDevice(&g_device);
    a.Close();
    return false;
  }

  m_hwVer = static_cast<int>(g_device.hwVer);

  g_params->devParams->fsFreq.fsHz = kFsHz;
  auto *rx = g_params->rxChannelA;
  rx->tunerParams.rfFreq.rfHz = static_cast<double>(kDefaultFreqHz);
  rx->tunerParams.bwType = sdrplay_api_BW_0_600;
  rx->tunerParams.ifType = sdrplay_api_IF_Zero;
  rx->tunerParams.gain.gRdB = 40;
  // Strong broadcast FM overloads the front end at LNAstate 0; back the LNA off
  // a few steps by default (AGC still trims the IF gain).
  rx->tunerParams.gain.LNAstate = 4;
  rx->ctrlParams.decimation.enable = 1;
  rx->ctrlParams.decimation.decimationFactor = kDecim;
  rx->ctrlParams.agc.enable = sdrplay_api_AGC_50HZ; // auto gain by default
  rx->ctrlParams.agc.setPoint_dBfs = -30;
  m_inputRate = static_cast<int>(kFsHz / kDecim);

  sdrplay_api_CallbackFnsT cbs;
  cbs.StreamACbFn = streamA;
  cbs.StreamBCbFn = nullptr;
  cbs.EventCbFn = eventCb;
  sdrplay_api_ErrT ierr = a.Init(g_device.dev, &cbs, this);
  if (ierr != sdrplay_api_Success) {
    std::fprintf(stderr, "[SDRplay] Init failed: %d\n", static_cast<int>(ierr));
    a.ReleaseDevice(&g_device);
    a.Close();
    return false;
  }
  float apiVer = 0.0f;
  if (a.ApiVersion) a.ApiVersion(&apiVer);
  std::fprintf(stderr,
               "[SDRplay] %s (SerNo %s) API %.2f | %g MHz/%d = %d Hz | antennas "
               "%d | LNAstate %d + AGC\n",
               modelName(), g_device.SerNo, apiVer, kFsHz / 1e6, kDecim,
               m_inputRate, antennaCount(), rx->tunerParams.gain.LNAstate);
  m_handle = g_device.dev;
  {
    std::lock_guard<std::mutex> lk(m_mutex);
    m_readPos = m_writePos = 0;
    m_full = false;
  }
  m_connected.store(true, std::memory_order_relaxed);
  m_failed.store(false, std::memory_order_relaxed);
  return true;
}

void SDRplayDevice::disconnect() {
  if (!m_connected.exchange(false)) return;
  auto &a = api();
  if (a.ok()) {
    if (m_handle) a.Uninit(m_handle);
    a.ReleaseDevice(&g_device);
    a.Close();
  }
  m_handle = nullptr;
  g_params = nullptr;
}

bool SDRplayDevice::setFrequency(uint32_t freqHz) {
  if (!m_connected.load() || !g_params || !m_handle) return false;
  g_params->rxChannelA->tunerParams.rfFreq.rfHz = static_cast<double>(freqHz);
  return api().Update(m_handle, g_device.tuner, sdrplay_api_Update_Tuner_Frf,
                      sdrplay_api_Update_Ext1_None) == sdrplay_api_Success;
}

bool SDRplayDevice::setSampleRate(uint32_t rate) {
  // The RSP rate is fixed at connect (2 MHz / 8). Accept the matching request;
  // reject mismatches so the caller notices a misconfiguration.
  return rate == 0 || static_cast<int>(rate) == m_inputRate;
}

bool SDRplayDevice::setFrequencyCorrection(int) {
  // The RSP TCXO is accurate enough for broadcast FM; no ppm trim applied.
  return true;
}

bool SDRplayDevice::setAGC(bool enable) {
  if (!m_connected.load() || !g_params || !m_handle) return false;
  g_params->rxChannelA->ctrlParams.agc.enable =
      enable ? sdrplay_api_AGC_50HZ : sdrplay_api_AGC_DISABLE;
  return api().Update(m_handle, g_device.tuner, sdrplay_api_Update_Ctrl_Agc,
                      sdrplay_api_Update_Ext1_None) == sdrplay_api_Success;
}

bool SDRplayDevice::setGainMode(bool manual) { return setAGC(!manual); }

bool SDRplayDevice::setGain(uint32_t gainTenthsDb) {
  if (!m_connected.load() || !g_params || !m_handle) return false;
  const double gainDb = static_cast<double>(gainTenthsDb) / 10.0;
  // Manual IF gain: disable AGC and map 0..50 dB "gain" to the IF gain
  // reduction gRdB (~20..59 dB; higher reduction = less gain).
  g_params->rxChannelA->ctrlParams.agc.enable = sdrplay_api_AGC_DISABLE;
  int gr = static_cast<int>(std::lround(59.0 - gainDb * 39.0 / 50.0));
  gr = std::max(20, std::min(59, gr));
  g_params->rxChannelA->tunerParams.gain.gRdB = gr;
  auto &a = api();
  a.Update(m_handle, g_device.tuner, sdrplay_api_Update_Ctrl_Agc,
           sdrplay_api_Update_Ext1_None);
  return a.Update(m_handle, g_device.tuner, sdrplay_api_Update_Tuner_Gr,
                  sdrplay_api_Update_Ext1_None) == sdrplay_api_Success;
}

bool SDRplayDevice::setBandwidthHz(int hz) {
  if (!m_connected.load() || !g_params || !m_handle) return false;
  g_params->rxChannelA->tunerParams.bwType = bwForHz(hz);
  return api().Update(m_handle, g_device.tuner, sdrplay_api_Update_Tuner_BwType,
                      sdrplay_api_Update_Ext1_None) == sdrplay_api_Success;
}

const char *SDRplayDevice::modelName() const {
  switch (m_hwVer) {
  case SDRPLAY_RSP1_ID: return "RSP1";
  case SDRPLAY_RSP1A_ID: return "RSP1A";
  case SDRPLAY_RSP1B_ID: return "RSP1B";
  case SDRPLAY_RSP2_ID: return "RSP2";
  case SDRPLAY_RSPduo_ID: return "RSPduo";
  case SDRPLAY_RSPdx_ID: return "RSPdx";
  case SDRPLAY_RSPdxR2_ID: return "RSPdx R2";
  default: return "RSP";
  }
}

bool SDRplayDevice::setLnaState(int state) {
  if (!m_connected.load() || !g_params || !m_handle) return false;
  g_params->rxChannelA->tunerParams.gain.LNAstate =
      static_cast<unsigned char>(std::max(0, std::min(27, state)));
  return api().Update(m_handle, g_device.tuner, sdrplay_api_Update_Tuner_Gr,
                      sdrplay_api_Update_Ext1_None) == sdrplay_api_Success;
}

bool SDRplayDevice::setScanWideMode(bool wide) {
  if (!m_connected.load() || !g_params || !m_handle) return false;
  auto *rx = g_params->rxChannelA;
  rx->ctrlParams.decimation.enable = 1;
  rx->ctrlParams.decimation.decimationFactor =
      static_cast<unsigned char>(wide ? 1 : kDecim);
  // Widen the IF analog filter so the undecimated span isn't filtered; restore
  // the FM channel default when leaving wide mode.
  rx->tunerParams.bwType = wide ? sdrplay_api_BW_1_536 : sdrplay_api_BW_0_600;
  auto &a = api();
  const bool okDecim =
      a.Update(m_handle, g_device.tuner, sdrplay_api_Update_Ctrl_Decimation,
               sdrplay_api_Update_Ext1_None) == sdrplay_api_Success;
  const bool okBw =
      a.Update(m_handle, g_device.tuner, sdrplay_api_Update_Tuner_BwType,
               sdrplay_api_Update_Ext1_None) == sdrplay_api_Success;
  m_inputRate =
      wide ? static_cast<int>(kFsHz) : static_cast<int>(kFsHz / kDecim);
  {
    std::lock_guard<std::mutex> lk(m_mutex);
    m_readPos = m_writePos = 0;
    m_full = false;
  }
  return okDecim && okBw;
}

int SDRplayDevice::antennaCount() const {
  switch (m_hwVer) {
  case SDRPLAY_RSPdx_ID:
  case SDRPLAY_RSPdxR2_ID: return 3;
  case SDRPLAY_RSP2_ID: return 2;
  default: return 1;
  }
}

bool SDRplayDevice::setAntenna(int index) {
  if (!m_connected.load() || !g_params || !m_handle) return false;
  auto &a = api();
  if (m_hwVer == SDRPLAY_RSPdx_ID || m_hwVer == SDRPLAY_RSPdxR2_ID) {
    const int idx = std::max(0, std::min(2, index));
    g_params->devParams->rspDxParams.antennaSel =
        static_cast<sdrplay_api_RspDx_AntennaSelectT>(idx);
    return a.Update(m_handle, g_device.tuner, sdrplay_api_Update_None,
                    sdrplay_api_Update_RspDx_AntennaControl) == sdrplay_api_Success;
  }
  return false;
}

bool SDRplayDevice::setBiasTee(bool enable) {
  if (!m_connected.load() || !g_params || !m_handle) return false;
  auto &a = api();
  if (m_hwVer == SDRPLAY_RSPdx_ID || m_hwVer == SDRPLAY_RSPdxR2_ID) {
    g_params->devParams->rspDxParams.biasTEnable = enable ? 1 : 0;
    return a.Update(m_handle, g_device.tuner, sdrplay_api_Update_None,
                    sdrplay_api_Update_RspDx_BiasTControl) == sdrplay_api_Success;
  }
  return false;
}

void SDRplayDevice::ingest(const short *xi, const short *xq, unsigned int n,
                           bool reset) {
  if (!xi || !xq || n == 0) return;
  std::lock_guard<std::mutex> lk(m_mutex);
  const size_t cap = m_ring.size();
  // On a retune the API raises `reset`; drop the buffered old-frequency IQ so
  // the new station is heard immediately instead of after the ring drains.
  if (reset) {
    m_readPos = m_writePos = 0;
    m_full = false;
  }
  for (unsigned int i = 0; i < n; i++) {
    m_ring[m_writePos] = std::complex<float>(xi[i] / 32768.0f, xq[i] / 32768.0f);
    m_writePos = (m_writePos + 1) % cap;
    if (m_full) m_readPos = (m_readPos + 1) % cap; // overwrite oldest
    if (m_writePos == m_readPos) m_full = true;
  }
  m_cv.notify_one();
}

size_t SDRplayDevice::drainLocked(std::complex<float> *out, size_t maxSamples) {
  const size_t cap = m_ring.size();
  auto avail = [&]() -> size_t {
    if (m_full) return cap;
    return (m_writePos + cap - m_readPos) % cap;
  };
  if (avail() == 0) {
    return 0;
  }
  size_t got = std::min(maxSamples, avail());
  for (size_t i = 0; i < got; i++) {
    out[i] = m_ring[m_readPos];
    m_readPos = (m_readPos + 1) % cap;
  }
  if (got > 0) m_full = false;
  return got;
}

size_t SDRplayDevice::readIQ(std::complex<float> *out, size_t maxSamples) {
  if (!out || maxSamples == 0) return 0;
  std::unique_lock<std::mutex> lk(m_mutex);
  const size_t cap = m_ring.size();
  auto avail = [&]() -> size_t {
    return m_full ? cap : (m_writePos + cap - m_readPos) % cap;
  };
  // Wait until a full block has accumulated (bounded by a ceiling). A tiny
  // partial read is harmless for the steady-state audio path, but it starves
  // the spectral scan: each scan retune raises the SDRplay `reset` flag and
  // flushes the ring, so an immediate short read returns too few samples for
  // the FFT (nfft < 1024 -> channel skipped -> fallback re-scan), which on a
  // full-band sweep stalls the main loop for many seconds (perceived as a
  // hang, with audio frozen). Blocking until the ring refills (~maxSamples/Fs,
  // ~32 ms for an 8192-sample block at 256 kHz) keeps captures whole and the
  // scan prompt; the predicate returns early in steady state so this adds no
  // latency once the stream is flowing.
  if (avail() < maxSamples) {
    m_cv.wait_for(lk, std::chrono::milliseconds(250),
                  [&] { return avail() >= maxSamples; });
  }
  return drainLocked(out, maxSamples);
}

size_t SDRplayDevice::readIQ(uint8_t *out, size_t maxSamples) {
  if (!out || maxSamples == 0) return 0;
  static thread_local std::vector<std::complex<float>> tmp;
  if (tmp.size() < maxSamples) tmp.resize(maxSamples);
  const size_t got = readIQ(tmp.data(), maxSamples);
  for (size_t i = 0; i < got; i++) {
    auto quant = [](float v) -> uint8_t {
      const float s = std::lround(v * 127.5f + 127.5f);
      return static_cast<uint8_t>(std::clamp(s, 0.0f, 255.0f));
    };
    out[2 * i] = quant(tmp[i].real());
    out[2 * i + 1] = quant(tmp[i].imag());
  }
  return got;
}

#else // FM_TUNER_HAS_SDRPLAY not defined: stub (no SDRplay SDK at build time)

SDRplayDevice::SDRplayDevice(uint32_t) {}
SDRplayDevice::~SDRplayDevice() {}
bool SDRplayDevice::apiAvailable() { return false; }
int SDRplayDevice::deviceCount() { return 0; }
bool SDRplayDevice::connect() { return false; }
void SDRplayDevice::disconnect() {}
bool SDRplayDevice::setFrequency(uint32_t) { return false; }
bool SDRplayDevice::setSampleRate(uint32_t) { return false; }
bool SDRplayDevice::setFrequencyCorrection(int) { return false; }
bool SDRplayDevice::setGainMode(bool) { return false; }
bool SDRplayDevice::setGain(uint32_t) { return false; }
bool SDRplayDevice::setAGC(bool) { return false; }
bool SDRplayDevice::setBandwidthHz(int) { return false; }
bool SDRplayDevice::setLnaState(int) { return false; }
bool SDRplayDevice::setScanWideMode(bool) { return false; }
bool SDRplayDevice::setAntenna(int) { return false; }
bool SDRplayDevice::setBiasTee(bool) { return false; }
int SDRplayDevice::antennaCount() const { return 1; }
const char *SDRplayDevice::modelName() const { return "RSP"; }
void SDRplayDevice::ingest(const short *, const short *, unsigned int, bool) {}
size_t SDRplayDevice::drainLocked(std::complex<float> *, size_t) { return 0; }
size_t SDRplayDevice::readIQ(std::complex<float> *, size_t) { return 0; }
size_t SDRplayDevice::readIQ(uint8_t *, size_t) { return 0; }

#endif
