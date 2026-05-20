#include "mpx_audio_output.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>

#if defined(__APPLE__) && defined(FM_TUNER_HAS_COREAUDIO)
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#endif

namespace {
constexpr size_t kMpxQueueSecondsAtTarget = 1; // ~1 s of buffered MPX

#if defined(__APPLE__) && defined(FM_TUNER_HAS_COREAUDIO)
std::string cfStringToStdString(CFStringRef str) {
  if (!str) return {};
  const CFIndex length = CFStringGetLength(str);
  const CFIndex maxSize =
      CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8) + 1;
  std::string result(static_cast<size_t>(maxSize), '\0');
  if (CFStringGetCString(str, result.data(), maxSize, kCFStringEncodingUTF8) ==
      0) {
    return {};
  }
  result.resize(std::strlen(result.c_str()));
  return result;
}

std::string deviceNameForId(AudioDeviceID id) {
  CFStringRef cfName = nullptr;
  UInt32 size = sizeof(cfName);
  AudioObjectPropertyAddress addr{kAudioObjectPropertyName,
                                  kAudioObjectPropertyScopeGlobal,
                                  kAudioObjectPropertyElementMain};
  if (AudioObjectGetPropertyData(id, &addr, 0, nullptr, &size, &cfName) !=
          noErr ||
      !cfName) {
    return {};
  }
  std::string name = cfStringToStdString(cfName);
  CFRelease(cfName);
  return name;
}

bool deviceHasOutputChannels(AudioDeviceID id) {
  AudioObjectPropertyAddress addr{kAudioDevicePropertyStreamConfiguration,
                                  kAudioDevicePropertyScopeOutput,
                                  kAudioObjectPropertyElementWildcard};
  UInt32 size = 0;
  if (AudioObjectGetPropertyDataSize(id, &addr, 0, nullptr, &size) != noErr ||
      size == 0) {
    return false;
  }
  std::vector<uint8_t> storage(size);
  auto *bufferList = reinterpret_cast<AudioBufferList *>(storage.data());
  if (AudioObjectGetPropertyData(id, &addr, 0, nullptr, &size, bufferList) !=
      noErr) {
    return false;
  }
  UInt32 channels = 0;
  for (UInt32 i = 0; i < bufferList->mNumberBuffers; i++) {
    channels += bufferList->mBuffers[i].mNumberChannels;
  }
  return channels > 0;
}

AudioDeviceID findOutputDevice(const std::string &selector) {
  AudioObjectPropertyAddress addr{kAudioHardwarePropertyDevices,
                                  kAudioObjectPropertyScopeGlobal,
                                  kAudioObjectPropertyElementMain};
  UInt32 size = 0;
  if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &addr, 0,
                                     nullptr, &size) != noErr ||
      size == 0) {
    return kAudioObjectUnknown;
  }
  std::vector<AudioDeviceID> devices(size / sizeof(AudioDeviceID));
  if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr,
                                 &size, devices.data()) != noErr) {
    return kAudioObjectUnknown;
  }
  std::vector<AudioDeviceID> outputs;
  for (AudioDeviceID id : devices) {
    if (id != kAudioObjectUnknown && deviceHasOutputChannels(id)) {
      outputs.push_back(id);
    }
  }
  if (outputs.empty()) return kAudioObjectUnknown;

  if (selector.empty()) {
    // System default output.
    AudioDeviceID def = kAudioObjectUnknown;
    UInt32 defSize = sizeof(def);
    AudioObjectPropertyAddress defAddr{kAudioHardwarePropertyDefaultOutputDevice,
                                       kAudioObjectPropertyScopeGlobal,
                                       kAudioObjectPropertyElementMain};
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &defAddr, 0,
                                   nullptr, &defSize, &def) == noErr &&
        def != kAudioObjectUnknown) {
      return def;
    }
    return outputs.front();
  }

  std::string needle = selector;
  std::transform(needle.begin(), needle.end(), needle.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  for (AudioDeviceID id : outputs) {
    std::string name = deviceNameForId(id);
    std::string nameLower = name;
    std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (!nameLower.empty() && nameLower.find(needle) != std::string::npos) {
      return id;
    }
  }
  return kAudioObjectUnknown;
}
#endif
} // namespace

MpxAudioOutput::MpxAudioOutput()
    : m_sourceRate(0), m_targetRate(0), m_verboseLogging(false),
      m_running(false), m_resampleEnabled(false), m_readPos(0), m_writePos(0),
      m_size(0)
#if defined(__APPLE__) && defined(FM_TUNER_HAS_COREAUDIO)
      ,
      m_audioUnit(nullptr)
#endif
#if defined(__linux__) && defined(FM_TUNER_HAS_ALSA)
      ,
      m_alsaPcm(nullptr), m_alsaThreadRunning(false)
#endif
#if defined(_WIN32) && defined(FM_TUNER_HAS_WINMM)
      ,
      m_waveOut(nullptr), m_winmmThreadRunning(false)
#endif
{
}

MpxAudioOutput::~MpxAudioOutput() { shutdown(); }

void MpxAudioOutput::clearQueueLocked() {
  m_readPos = 0;
  m_writePos = 0;
  m_size = 0;
}

bool MpxAudioOutput::enqueueMpx(const float *samples, size_t sampleCount) {
  if (!samples || sampleCount == 0 || !m_running.load() || m_ring.empty()) {
    return false;
  }

  // Resample on the producer thread so the audio backend pulls already-at-rate.
  static thread_local std::vector<float> rsAccum;
  rsAccum.clear();

  if (m_resampleEnabled) {
    rsAccum.reserve(sampleCount);
    for (size_t i = 0; i < sampleCount; i++) {
      const uint32_t produced =
          m_resampler.execute(samples[i], m_resampleTmp);
      for (uint32_t p = 0; p < produced; p++) {
        rsAccum.push_back(m_resampleTmp[p]);
      }
    }
    if (rsAccum.empty()) {
      return true; // resampler hasn't produced anything yet (common downsample)
    }
    samples = rsAccum.data();
    sampleCount = rsAccum.size();
  }

  std::lock_guard<std::mutex> lock(m_mutex);
  const size_t capacity = m_ring.size();
  size_t start = 0;
  if (sampleCount >= capacity) {
    start = sampleCount - capacity;
  }
  const size_t kept = sampleCount - start;
  if (m_size + kept > capacity) {
    size_t drop = m_size + kept - capacity;
    drop = std::min(drop, m_size);
    m_readPos = (m_readPos + drop) % capacity;
    m_size -= drop;
    if (m_verboseLogging) {
      static std::atomic<uint32_t> overflowCount{0};
      const uint32_t count = ++overflowCount;
      if (count <= 5 || (count % 50) == 0) {
        std::cerr << "[MPX-AUDIO] queue overflow (" << count << ")\n";
      }
    }
  }
  for (size_t i = start; i < sampleCount; i++) {
    m_ring[m_writePos] = samples[i];
    m_writePos = (m_writePos + 1) % capacity;
  }
  m_size += kept;
  m_cv.notify_one();
  return true;
}

#if defined(__APPLE__) && defined(FM_TUNER_HAS_COREAUDIO)
OSStatus MpxAudioOutput::coreAudioRenderCallback(
    void *inRefCon, AudioUnitRenderActionFlags *, const AudioTimeStamp *,
    UInt32, UInt32 inNumberFrames, AudioBufferList *ioData) {
  auto *self = reinterpret_cast<MpxAudioOutput *>(inRefCon);
  if (!self || !ioData || inNumberFrames == 0) return noErr;
  // Non-interleaved float layout: mNumberBuffers == channel count, each
  // buffer is one mono channel's worth of frames. Write the same MPX sample
  // into every buffer so the device's L and R both carry identical mono MPX.
  float *outA = (ioData->mNumberBuffers > 0)
                    ? reinterpret_cast<float *>(ioData->mBuffers[0].mData)
                    : nullptr;
  float *outB = (ioData->mNumberBuffers > 1)
                    ? reinterpret_cast<float *>(ioData->mBuffers[1].mData)
                    : nullptr;
  if (!outA) return noErr;

  std::lock_guard<std::mutex> lock(self->m_mutex);
  const size_t available =
      std::min<size_t>(self->m_size, static_cast<size_t>(inNumberFrames));
  for (size_t i = 0; i < available; i++) {
    const float v = self->m_ring[self->m_readPos];
    self->m_readPos = (self->m_readPos + 1) % self->m_ring.size();
    outA[i] = v;
    if (outB) outB[i] = v;
  }
  for (size_t i = available; i < static_cast<size_t>(inNumberFrames); i++) {
    outA[i] = 0.0f;
    if (outB) outB[i] = 0.0f;
  }
  self->m_size -= available;
  return noErr;
}
#endif

#if defined(__linux__) && defined(FM_TUNER_HAS_ALSA)
void MpxAudioOutput::runAlsaThread() {
  constexpr size_t kWriteFrames = 1024;
  std::vector<int16_t> interleaved(kWriteFrames, 0);
  while (m_alsaThreadRunning.load()) {
    size_t copied = 0;
    {
      std::unique_lock<std::mutex> lock(m_mutex);
      m_cv.wait_for(lock, std::chrono::milliseconds(10), [&]() {
        return !m_alsaThreadRunning.load() || m_size > 0;
      });
      if (!m_alsaThreadRunning.load()) break;
      copied = std::min(kWriteFrames, m_size);
      for (size_t i = 0; i < copied; i++) {
        const float v = std::clamp(m_ring[m_readPos], -1.0f, 1.0f);
        interleaved[i] = static_cast<int16_t>(v * 32767.0f);
        m_readPos = (m_readPos + 1) % m_ring.size();
      }
      m_size -= copied;
    }
    if (copied == 0) continue;
    snd_pcm_sframes_t written =
        snd_pcm_writei(m_alsaPcm, interleaved.data(), copied);
    if (written < 0) {
      written = snd_pcm_recover(m_alsaPcm, static_cast<int>(written), 1);
      if (written < 0 && m_verboseLogging) {
        std::cerr << "[MPX-AUDIO] ALSA write recovery failed: "
                  << snd_strerror(static_cast<int>(written)) << "\n";
      }
    }
  }
}
#endif

#if defined(_WIN32) && defined(FM_TUNER_HAS_WINMM)
void MpxAudioOutput::runWinMMThread() {
  constexpr size_t kFrames = 1024;
  constexpr size_t kNumBuffers = 4;
  std::vector<std::vector<int16_t>> buffers(
      kNumBuffers, std::vector<int16_t>(kFrames, 0));
  std::vector<WAVEHDR> headers(kNumBuffers);
  for (size_t i = 0; i < kNumBuffers; i++) {
    std::memset(&headers[i], 0, sizeof(WAVEHDR));
    headers[i].lpData = reinterpret_cast<LPSTR>(buffers[i].data());
    headers[i].dwBufferLength =
        static_cast<DWORD>(kFrames * sizeof(int16_t));
    const MMRESULT pr =
        waveOutPrepareHeader(m_waveOut, &headers[i], sizeof(WAVEHDR));
    if (pr != MMSYSERR_NOERROR) {
      if (m_verboseLogging) {
        std::cerr << "[MPX-AUDIO] WinMM prepare header failed: " << pr << "\n";
      }
      for (size_t j = 0; j < i; j++) {
        waveOutUnprepareHeader(m_waveOut, &headers[j], sizeof(WAVEHDR));
      }
      m_winmmThreadRunning = false;
      return;
    }
  }

  while (m_winmmThreadRunning.load()) {
    WAVEHDR *hdr = nullptr;
    std::vector<int16_t> *pcm = nullptr;
    for (size_t i = 0; i < kNumBuffers; i++) {
      if ((headers[i].dwFlags & WHDR_INQUEUE) == 0) {
        hdr = &headers[i];
        pcm = &buffers[i];
        break;
      }
    }
    if (!hdr) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      continue;
    }

    size_t copied = 0;
    {
      std::unique_lock<std::mutex> lock(m_mutex);
      m_cv.wait_for(lock, std::chrono::milliseconds(10), [&]() {
        return !m_winmmThreadRunning.load() || m_size > 0;
      });
      if (!m_winmmThreadRunning.load()) break;
      copied = std::min(kFrames, m_size);
      for (size_t i = 0; i < copied; i++) {
        const float v = std::clamp(m_ring[m_readPos], -1.0f, 1.0f);
        (*pcm)[i] = static_cast<int16_t>(v * 32767.0f);
        m_readPos = (m_readPos + 1) % m_ring.size();
      }
      m_size -= copied;
      for (size_t i = copied; i < kFrames; i++) (*pcm)[i] = 0;
    }
    hdr->dwBufferLength = static_cast<DWORD>(kFrames * sizeof(int16_t));
    waveOutWrite(m_waveOut, hdr, sizeof(WAVEHDR));
  }

  if (m_waveOut) {
    waveOutReset(m_waveOut);
  }
  for (size_t i = 0; i < kNumBuffers; i++) {
    waveOutUnprepareHeader(m_waveOut, &headers[i], sizeof(WAVEHDR));
  }
}
#endif

bool MpxAudioOutput::init(const std::string &deviceSelector,
                          uint32_t sourceSampleRate,
                          uint32_t targetSampleRate, bool verboseLogging) {
  shutdown();

  if (sourceSampleRate == 0 || targetSampleRate == 0) {
    std::cerr << "[MPX-AUDIO] init: sample rate must be > 0\n";
    return false;
  }

  m_sourceRate = sourceSampleRate;
  m_targetRate = targetSampleRate;
  m_verboseLogging = verboseLogging;
  m_resampleEnabled = false;

  if (sourceSampleRate != targetSampleRate) {
    const float ratio =
        static_cast<float>(targetSampleRate) / static_cast<float>(sourceSampleRate);
    try {
      m_resampler.init(ratio);
      m_resampleEnabled = true;
    } catch (const std::exception &ex) {
      std::cerr << "[MPX-AUDIO] resampler init failed: " << ex.what() << "\n";
      return false;
    }
  }

  m_ring.assign(
      static_cast<size_t>(targetSampleRate) * kMpxQueueSecondsAtTarget, 0.0f);
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    clearQueueLocked();
  }

#if defined(__APPLE__) && defined(FM_TUNER_HAS_COREAUDIO)
  AudioComponentDescription desc{};
  desc.componentType = kAudioUnitType_Output;
  desc.componentSubType = kAudioUnitSubType_HALOutput;
  desc.componentManufacturer = kAudioUnitManufacturer_Apple;
  AudioComponent component = AudioComponentFindNext(nullptr, &desc);
  if (!component) {
    std::cerr << "[MPX-AUDIO] CoreAudio output component not found\n";
    return false;
  }
  if (AudioComponentInstanceNew(component, &m_audioUnit) != noErr ||
      !m_audioUnit) {
    std::cerr << "[MPX-AUDIO] CoreAudio instance creation failed\n";
    m_audioUnit = nullptr;
    return false;
  }
  UInt32 enableIO = 1;
  AudioUnitSetProperty(m_audioUnit, kAudioOutputUnitProperty_EnableIO,
                       kAudioUnitScope_Output, 0, &enableIO, sizeof(enableIO));

  const AudioDeviceID devId = findOutputDevice(deviceSelector);
  if (devId == kAudioObjectUnknown) {
    std::cerr << "[MPX-AUDIO] CoreAudio device not found for selector: '"
              << deviceSelector << "'\n";
    AudioComponentInstanceDispose(m_audioUnit);
    m_audioUnit = nullptr;
    return false;
  }
  if (verboseLogging) {
    std::cout << "[MPX-AUDIO] CoreAudio device: '" << deviceNameForId(devId)
              << "' (selector '" << deviceSelector << "')\n";
  }
  if (AudioUnitSetProperty(m_audioUnit,
                           kAudioOutputUnitProperty_CurrentDevice,
                           kAudioUnitScope_Global, 0, &devId,
                           sizeof(devId)) != noErr) {
    std::cerr << "[MPX-AUDIO] CoreAudio failed to select device\n";
    AudioComponentInstanceDispose(m_audioUnit);
    m_audioUnit = nullptr;
    return false;
  }

  // Try mono first (cheapest), fall back to stereo (duplicating L=R=MPX) if
  // the device rejects a 1-channel StreamFormat. USB DACs vary — virtual
  // loopbacks like BlackHole accept mono fine, but some hardware insists on
  // 2-channel output and will fail the property set.
  auto trySetFormat = [&](unsigned int channels) {
    AudioStreamBasicDescription asbd{};
    asbd.mSampleRate = static_cast<Float64>(targetSampleRate);
    asbd.mFormatID = kAudioFormatLinearPCM;
    asbd.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked |
                        kAudioFormatFlagIsNonInterleaved;
    asbd.mFramesPerPacket = 1;
    asbd.mChannelsPerFrame = channels;
    asbd.mBitsPerChannel = 32;
    asbd.mBytesPerFrame = sizeof(float);
    asbd.mBytesPerPacket = sizeof(float);
    return AudioUnitSetProperty(m_audioUnit, kAudioUnitProperty_StreamFormat,
                                kAudioUnitScope_Input, 0, &asbd,
                                sizeof(asbd)) == noErr;
  };
  if (trySetFormat(1)) {
    m_outChannels = 1;
  } else if (trySetFormat(2)) {
    m_outChannels = 2;
    if (verboseLogging) {
      std::cout << "[MPX-AUDIO] device rejected mono format; using stereo "
                   "(MPX duplicated to L=R)\n";
    }
  } else {
    std::cerr << "[MPX-AUDIO] CoreAudio set stream format failed (device may "
                 "not support "
              << targetSampleRate << " Hz)\n";
    AudioComponentInstanceDispose(m_audioUnit);
    m_audioUnit = nullptr;
    return false;
  }

  AURenderCallbackStruct callback{};
  callback.inputProc = &MpxAudioOutput::coreAudioRenderCallback;
  callback.inputProcRefCon = this;
  if (AudioUnitSetProperty(m_audioUnit, kAudioUnitProperty_SetRenderCallback,
                           kAudioUnitScope_Input, 0, &callback,
                           sizeof(callback)) != noErr) {
    std::cerr << "[MPX-AUDIO] CoreAudio set render callback failed\n";
    AudioComponentInstanceDispose(m_audioUnit);
    m_audioUnit = nullptr;
    return false;
  }
  if (AudioUnitInitialize(m_audioUnit) != noErr ||
      AudioOutputUnitStart(m_audioUnit) != noErr) {
    std::cerr << "[MPX-AUDIO] CoreAudio initialize/start failed\n";
    AudioUnitUninitialize(m_audioUnit);
    AudioComponentInstanceDispose(m_audioUnit);
    m_audioUnit = nullptr;
    return false;
  }
  m_running = true;
  if (verboseLogging) {
    std::cout << "[MPX-AUDIO] streaming mono @ " << targetSampleRate
              << " Hz (resampled from " << sourceSampleRate << " Hz)\n";
  }
  return true;

#elif defined(__linux__) && defined(FM_TUNER_HAS_ALSA)
  std::string alsaDevice =
      deviceSelector.empty() ? "default" : deviceSelector;
  int err = snd_pcm_open(&m_alsaPcm, alsaDevice.c_str(), SND_PCM_STREAM_PLAYBACK, 0);
  if (err < 0) {
    std::cerr << "[MPX-AUDIO] ALSA open '" << alsaDevice
              << "' failed: " << snd_strerror(err) << "\n";
    m_alsaPcm = nullptr;
    return false;
  }
  snd_pcm_hw_params_t *hw = nullptr;
  snd_pcm_hw_params_alloca(&hw);
  snd_pcm_hw_params_any(m_alsaPcm, hw);
  snd_pcm_hw_params_set_access(m_alsaPcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
  snd_pcm_hw_params_set_format(m_alsaPcm, hw, SND_PCM_FORMAT_S16_LE);
  snd_pcm_hw_params_set_channels(m_alsaPcm, hw, 1);
  unsigned int rate = targetSampleRate;
  int dir = 0;
  err = snd_pcm_hw_params_set_rate_near(m_alsaPcm, hw, &rate, &dir);
  if (err < 0) {
    std::cerr << "[MPX-AUDIO] ALSA set_rate failed: " << snd_strerror(err)
              << "\n";
    snd_pcm_close(m_alsaPcm);
    m_alsaPcm = nullptr;
    return false;
  }
  if (rate != targetSampleRate) {
    std::cerr << "[MPX-AUDIO] WARNING: device does not support "
              << targetSampleRate << " Hz; using " << rate
              << " Hz (MPX will be band-limited to " << (rate / 2)
              << " Hz)\n";
    m_targetRate = rate;
  }
  snd_pcm_uframes_t buf = 8192;
  snd_pcm_hw_params_set_buffer_size_near(m_alsaPcm, hw, &buf);
  snd_pcm_uframes_t period = 1024;
  snd_pcm_hw_params_set_period_size_near(m_alsaPcm, hw, &period, &dir);
  err = snd_pcm_hw_params(m_alsaPcm, hw);
  if (err < 0) {
    std::cerr << "[MPX-AUDIO] ALSA hw_params failed: " << snd_strerror(err)
              << "\n";
    snd_pcm_close(m_alsaPcm);
    m_alsaPcm = nullptr;
    return false;
  }
  m_running = true;
  m_alsaThreadRunning = true;
  m_alsaThread = std::thread(&MpxAudioOutput::runAlsaThread, this);
  if (verboseLogging) {
    std::cout << "[MPX-AUDIO] streaming mono via ALSA '" << alsaDevice
              << "' @ " << m_targetRate << " Hz (resampled from "
              << sourceSampleRate << " Hz)\n";
  }
  return true;

#elif defined(_WIN32) && defined(FM_TUNER_HAS_WINMM)
  (void)deviceSelector;
  // WinMM (the only built-in Windows audio backend in this project) is a
  // legacy API capped at 48 kHz on the common configurations. At that rate
  // the 19/38/57 kHz subcarriers fold into the audible band and the MPX
  // becomes useless for downstream tools. Refuse the path here so the user
  // gets a clear error instead of silently broken output; --mpx-wav still
  // works for capture, and Linux/macOS support the live-audio path.
  std::cerr << "[MPX-AUDIO] not supported on Windows: WinMM is capped at "
               "48 kHz which aliases the 19/38/57 kHz subcarriers. "
               "Use --mpx-wav for file capture instead, or switch to a "
               "Linux/macOS host for live MPX-to-audio output.\n";
  return false;
#else
  (void)deviceSelector;
  std::cerr << "[MPX-AUDIO] no audio backend compiled in\n";
  return false;
#endif
}

void MpxAudioOutput::shutdown() {
  m_running = false;

#if defined(__APPLE__) && defined(FM_TUNER_HAS_COREAUDIO)
  if (m_audioUnit) {
    AudioOutputUnitStop(m_audioUnit);
    AudioUnitUninitialize(m_audioUnit);
    AudioComponentInstanceDispose(m_audioUnit);
    m_audioUnit = nullptr;
  }
#endif
#if defined(__linux__) && defined(FM_TUNER_HAS_ALSA)
  m_alsaThreadRunning = false;
  m_cv.notify_all();
  if (m_alsaThread.joinable()) m_alsaThread.join();
  if (m_alsaPcm) {
    snd_pcm_drop(m_alsaPcm);
    snd_pcm_close(m_alsaPcm);
    m_alsaPcm = nullptr;
  }
#endif
#if defined(_WIN32) && defined(FM_TUNER_HAS_WINMM)
  m_winmmThreadRunning = false;
  m_cv.notify_all();
  if (m_winmmThread.joinable()) m_winmmThread.join();
  if (m_waveOut) {
    waveOutClose(m_waveOut);
    m_waveOut = nullptr;
  }
#endif
}
