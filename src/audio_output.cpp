#include "audio_output.h"
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

#if defined(__linux__) && defined(FM_TUNER_HAS_ALSA)
namespace {
bool parseHwDeviceGlobal(const std::string &selector, int &card, int &device) {
  if (selector.size() < 5 || selector.substr(0, 3) != "hw:") {
    return false;
  }
  size_t comma = selector.find(',', 3);
  if (comma == std::string::npos) {
    return false;
  }
  std::string cardStr = selector.substr(3, comma - 3);
  std::string devStr = selector.substr(comma + 1);
  if (cardStr.empty() || devStr.empty()) {
    return false;
  }
  for (char c : cardStr) {
    if (c < '0' || c > '9')
      return false;
  }
  for (char c : devStr) {
    if (c < '0' || c > '9')
      return false;
  }
  card = std::stoi(cardStr);
  device = std::stoi(devStr);
  return true;
}
} // namespace
#endif

#if defined(__APPLE__) && defined(FM_TUNER_HAS_COREAUDIO)
namespace {
std::string trimCoreAudio(const std::string &value) {
  size_t start = 0;
  while (start < value.size() &&
         std::isspace(static_cast<unsigned char>(value[start])) != 0) {
    start++;
  }
  size_t end = value.size();
  while (end > start &&
         std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
    end--;
  }
  return value.substr(start, end - start);
}

std::string normalizeCoreAudioSelector(const std::string &rawSelector) {
  std::string selector = trimCoreAudio(rawSelector);
  if (selector.size() >= 2) {
    const char first = selector.front();
    const char last = selector.back();
    if ((first == '"' && last == '"') || (first == '\'' && last == '\'')) {
      selector = trimCoreAudio(selector.substr(1, selector.size() - 2));
    }
  }
  return selector;
}

bool parseCoreAudioDeviceIndex(const std::string &selector, int &outIndex) {
  if (selector.empty()) {
    return false;
  }
  for (char c : selector) {
    if (c < '0' || c > '9') {
      return false;
    }
  }
  outIndex = std::stoi(selector);
  return true;
}

std::string toLowerCoreAudio(std::string value) {
  std::transform(
      value.begin(), value.end(), value.begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

std::string cfStringToStdString(CFStringRef str) {
  if (!str) {
    return std::string();
  }
  const CFIndex length = CFStringGetLength(str);
  const CFIndex maxSize =
      CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8) + 1;
  std::string result(static_cast<size_t>(maxSize), '\0');
  if (CFStringGetCString(str, result.data(), maxSize, kCFStringEncodingUTF8) ==
      0) {
    return std::string();
  }
  result.resize(std::strlen(result.c_str()));
  return result;
}

bool deviceHasOutputChannels(AudioDeviceID deviceId) {
  AudioObjectPropertyAddress addr{kAudioDevicePropertyStreamConfiguration,
                                  kAudioDevicePropertyScopeOutput,
                                  kAudioObjectPropertyElementWildcard};
  UInt32 size = 0;
  if (AudioObjectGetPropertyDataSize(deviceId, &addr, 0, nullptr, &size) !=
          noErr ||
      size == 0) {
    return false;
  }
  std::vector<uint8_t> storage(size);
  AudioBufferList *bufferList =
      reinterpret_cast<AudioBufferList *>(storage.data());
  if (AudioObjectGetPropertyData(deviceId, &addr, 0, nullptr, &size,
                                 bufferList) != noErr) {
    return false;
  }
  UInt32 channels = 0;
  for (UInt32 i = 0; i < bufferList->mNumberBuffers; i++) {
    channels += bufferList->mBuffers[i].mNumberChannels;
  }
  return channels > 0;
}

std::vector<AudioDeviceID> enumerateOutputDevices() {
  AudioObjectPropertyAddress addr{kAudioHardwarePropertyDevices,
                                  kAudioObjectPropertyScopeGlobal,
                                  kAudioObjectPropertyElementMain};
  UInt32 size = 0;
  if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &addr, 0,
                                     nullptr, &size) != noErr ||
      size == 0) {
    return {};
  }
  std::vector<AudioDeviceID> devices(size / sizeof(AudioDeviceID));
  if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr,
                                 &size, devices.data()) != noErr) {
    return {};
  }
  std::vector<AudioDeviceID> outputs;
  outputs.reserve(devices.size());
  for (AudioDeviceID id : devices) {
    if (id != kAudioObjectUnknown && deviceHasOutputChannels(id)) {
      outputs.push_back(id);
    }
  }
  return outputs;
}

AudioDeviceID defaultOutputDevice() {
  AudioDeviceID device = kAudioObjectUnknown;
  UInt32 size = sizeof(device);
  AudioObjectPropertyAddress addr{kAudioHardwarePropertyDefaultOutputDevice,
                                  kAudioObjectPropertyScopeGlobal,
                                  kAudioObjectPropertyElementMain};
  if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr,
                                 &size, &device) != noErr) {
    return kAudioObjectUnknown;
  }
  return device;
}

std::string outputDeviceName(AudioDeviceID deviceId) {
  CFStringRef cfName = nullptr;
  UInt32 size = sizeof(cfName);
  AudioObjectPropertyAddress addr{kAudioObjectPropertyName,
                                  kAudioObjectPropertyScopeGlobal,
                                  kAudioObjectPropertyElementMain};
  if (AudioObjectGetPropertyData(deviceId, &addr, 0, nullptr, &size, &cfName) !=
          noErr ||
      !cfName) {
    return std::string();
  }
  std::string name = cfStringToStdString(cfName);
  CFRelease(cfName);
  return name;
}

AudioDeviceID selectOutputDeviceCoreAudio(const std::string &selector) {
  const auto devices = enumerateOutputDevices();
  if (devices.empty()) {
    return kAudioObjectUnknown;
  }
  if (selector.empty()) {
    const AudioDeviceID def = defaultOutputDevice();
    if (def != kAudioObjectUnknown) {
      return def;
    }
    return devices.front();
  }
  int requestedIndex = -1;
  if (parseCoreAudioDeviceIndex(selector, requestedIndex)) {
    if (requestedIndex >= 0 &&
        requestedIndex < static_cast<int>(devices.size())) {
      return devices[static_cast<size_t>(requestedIndex)];
    }
    return kAudioObjectUnknown;
  }
  const std::string needle = toLowerCoreAudio(selector);
  for (AudioDeviceID id : devices) {
    const std::string name = outputDeviceName(id);
    if (!name.empty() &&
        toLowerCoreAudio(name).find(needle) != std::string::npos) {
      return id;
    }
  }
  return kAudioObjectUnknown;
}
} // namespace
#endif

#if defined(_WIN32) && defined(FM_TUNER_HAS_WINMM)
namespace {
std::string narrowFromWide(const WCHAR *wide) {
  if (!wide || *wide == L'\0') {
    return std::string();
  }
  const int len =
      WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
  if (len <= 1) {
    return std::string();
  }
  std::string out(static_cast<size_t>(len - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), len, nullptr, nullptr);
  return out;
}

std::string trimWinMM(const std::string &value) {
  size_t start = 0;
  while (start < value.size() &&
         std::isspace(static_cast<unsigned char>(value[start])) != 0) {
    start++;
  }
  size_t end = value.size();
  while (end > start &&
         std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
    end--;
  }
  return value.substr(start, end - start);
}

bool parseWinMMIndex(const std::string &selector, UINT &index) {
  if (selector.empty()) {
    return false;
  }
  for (char c : selector) {
    if (c < '0' || c > '9') {
      return false;
    }
  }
  index = static_cast<UINT>(std::stoul(selector));
  return true;
}

std::string toLowerWinMM(std::string value) {
  std::transform(
      value.begin(), value.end(), value.begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

UINT selectWinMMDevice(const std::string &selector) {
  const UINT num = waveOutGetNumDevs();
  if (selector.empty()) {
    return WAVE_MAPPER;
  }
  UINT idx = 0;
  if (parseWinMMIndex(selector, idx)) {
    if (idx < num) {
      return idx;
    }
    return UINT_MAX;
  }
  const std::string needle = toLowerWinMM(selector);
  for (UINT i = 0; i < num; i++) {
    WAVEOUTCAPSW caps{};
    if (waveOutGetDevCapsW(i, &caps, sizeof(caps)) != MMSYSERR_NOERROR) {
      continue;
    }
    const std::string name = toLowerWinMM(narrowFromWide(caps.szPname));
    if (!name.empty() && name.find(needle) != std::string::npos) {
      return i;
    }
  }
  return UINT_MAX;
}
} // namespace
#endif

bool AudioOutput::listDevices() {
#if defined(__linux__) && defined(FM_TUNER_HAS_ALSA)
  return listAlsaDevices();
#elif defined(__APPLE__) && defined(FM_TUNER_HAS_COREAUDIO)
  return listCoreAudioDevices();
#elif defined(_WIN32) && defined(FM_TUNER_HAS_WINMM)
  return listWinMMDevices();
#else
  std::cerr << "Audio device listing is unavailable in this build\n";
  return false;
#endif
}

void AudioOutput::clearSpeakerQueueLocked() {
  m_speakerReadPos = 0;
  m_speakerWritePos = 0;
  m_speakerSize = 0;
}

void AudioOutput::logSpeakerOverflow(const char *backendLabel,
                                     uint32_t count) const {
  if (!m_verboseLogging) {
    return;
  }
  if (count <= 5 || (count % 50) == 0) {
    std::cerr << "[AUDIO] " << backendLabel << " queue overflow (" << count
              << ")\n";
  }
}

void AudioOutput::pushSpeakerSamples(const float *left, const float *right,
                                     size_t numSamples,
                                     const char *backendLabel) {
  if (!left || !right || numSamples == 0 || m_speakerRing.empty()) {
    return;
  }
  const size_t incomingSamples = numSamples * CHANNELS;
  const size_t capacity = m_speakerRing.size();
  size_t startSample = 0;
  if (incomingSamples >= capacity) {
    startSample = (incomingSamples - capacity) / CHANNELS;
  }
  const size_t keptSamples = (numSamples - startSample) * CHANNELS;

  std::lock_guard<std::mutex> lock(m_speakerMutex);
  if (keptSamples > capacity) {
    clearSpeakerQueueLocked();
    return;
  }
  if (m_speakerSize + keptSamples > capacity) {
    size_t drop = m_speakerSize + keptSamples - capacity;
    drop = std::min(drop, m_speakerSize);
    m_speakerReadPos = (m_speakerReadPos + drop) % capacity;
    m_speakerSize -= drop;
    static std::atomic<uint32_t> overflowCount{0};
    logSpeakerOverflow(backendLabel, ++overflowCount);
  }

  for (size_t i = startSample; i < numSamples; i++) {
    m_speakerRing[m_speakerWritePos] = left[i];
    m_speakerWritePos = (m_speakerWritePos + 1) % capacity;
    m_speakerRing[m_speakerWritePos] = right[i];
    m_speakerWritePos = (m_speakerWritePos + 1) % capacity;
  }
  m_speakerSize += keptSamples;
  m_speakerCv.notify_one();
}

size_t AudioOutput::popSpeakerSamplesLocked(float *dest, size_t maxSamples) {
  if (!dest || maxSamples == 0 || m_speakerRing.empty()) {
    return 0;
  }
  const size_t toRead = std::min(maxSamples, m_speakerSize);
  const size_t capacity = m_speakerRing.size();
  for (size_t i = 0; i < toRead; i++) {
    dest[i] = m_speakerRing[m_speakerReadPos];
    m_speakerReadPos = (m_speakerReadPos + 1) % capacity;
  }
  m_speakerSize -= toRead;
  return toRead;
}

#if defined(_WIN32) && defined(FM_TUNER_HAS_WINMM)
bool AudioOutput::listWinMMDevices() {
  const UINT num = waveOutGetNumDevs();
  std::cout << "Available audio output devices:\n";
  for (UINT i = 0; i < num; i++) {
    WAVEOUTCAPSW caps{};
    if (waveOutGetDevCapsW(i, &caps, sizeof(caps)) != MMSYSERR_NOERROR) {
      continue;
    }
    std::cout << "  [" << i << "] " << narrowFromWide(caps.szPname)
              << " (API: WinMM)\n";
  }
  return true;
}
#endif

#if defined(__APPLE__) && defined(FM_TUNER_HAS_COREAUDIO)
bool AudioOutput::listCoreAudioDevices() {
  const auto devices = enumerateOutputDevices();
  std::cout << "Available audio output devices:\n";
  for (size_t i = 0; i < devices.size(); i++) {
    const std::string name = outputDeviceName(devices[i]);
    std::cout << "  [" << i << "] " << (name.empty() ? "(unknown)" : name)
              << " (API: Core Audio)\n";
  }
  return true;
}

OSStatus AudioOutput::coreAudioRenderCallback(void *inRefCon,
                                              AudioUnitRenderActionFlags *,
                                              const AudioTimeStamp *, UInt32,
                                              UInt32 inNumberFrames,
                                              AudioBufferList *ioData) {
  auto *self = reinterpret_cast<AudioOutput *>(inRefCon);
  if (!self || !ioData || inNumberFrames == 0) {
    return noErr;
  }

  float *outA = (ioData->mNumberBuffers > 0)
                    ? reinterpret_cast<float *>(ioData->mBuffers[0].mData)
                    : nullptr;
  float *outB = (ioData->mNumberBuffers > 1)
                    ? reinterpret_cast<float *>(ioData->mBuffers[1].mData)
                    : nullptr;
  if (!outA) {
    return noErr;
  }

  std::lock_guard<std::mutex> lock(self->m_speakerMutex);
  const size_t samplePairs = std::min(self->m_speakerSize / 2,
                                      static_cast<size_t>(inNumberFrames));
  for (size_t i = 0; i < samplePairs; i++) {
    const float l = self->m_speakerRing[self->m_speakerReadPos];
    self->m_speakerReadPos =
        (self->m_speakerReadPos + 1) % self->m_speakerRing.size();
    const float r = self->m_speakerRing[self->m_speakerReadPos];
    self->m_speakerReadPos =
        (self->m_speakerReadPos + 1) % self->m_speakerRing.size();
    if (outB) {
      outA[i] = l;
      outB[i] = r;
    } else {
      outA[i * 2] = l;
      outA[i * 2 + 1] = r;
    }
  }
  for (size_t i = samplePairs; i < static_cast<size_t>(inNumberFrames); i++) {
    if (outB) {
      outA[i] = 0.0f;
      outB[i] = 0.0f;
    } else {
      outA[i * 2] = 0.0f;
      outA[i * 2 + 1] = 0.0f;
    }
  }
  self->m_speakerSize -= samplePairs * 2;
  return noErr;
}
#endif

#if defined(__linux__) && defined(FM_TUNER_HAS_ALSA)
bool AudioOutput::listAlsaDevices() {
  std::cout << "ALSA hardware devices:\n";
  int card = -1;
  bool hadErrors = false;
  bool listedAny = false;
  int rc = snd_card_next(&card);
  while (rc >= 0 && card >= 0) {
    char name[32];
    std::snprintf(name, sizeof(name), "hw:%d", card);
    snd_ctl_t *handle = nullptr;
    rc = snd_ctl_open(&handle, name, 0);
    if (rc >= 0) {
      snd_ctl_card_info_t *info = nullptr;
      snd_ctl_card_info_alloca(&info);
      rc = snd_ctl_card_info(handle, info);
      if (rc >= 0) {
        const char *cardName = snd_ctl_card_info_get_name(info);
        int device = -1;
        rc = snd_ctl_pcm_next_device(handle, &device);
        while (rc >= 0 && device >= 0) {
          snd_pcm_info_t *pcminfo = nullptr;
          snd_pcm_info_alloca(&pcminfo);
          snd_pcm_info_set_device(pcminfo, device);
          snd_pcm_info_set_subdevice(pcminfo, 0);
          snd_pcm_info_set_stream(pcminfo, SND_PCM_STREAM_PLAYBACK);
          rc = snd_ctl_pcm_info(handle, pcminfo);
          if (rc >= 0) {
            const char *devName = snd_pcm_info_get_name(pcminfo);
            std::cout << "  [hw:" << card << "," << device << "] " << cardName
                      << ": " << devName << "\n";
            listedAny = true;
          } else {
            hadErrors = true;
            std::cerr << "[AUDIO] ALSA snd_ctl_pcm_info(hw:" << card << ","
                      << device << ") failed: " << snd_strerror(rc) << "\n";
          }
          rc = snd_ctl_pcm_next_device(handle, &device);
        }
        if (rc < 0) {
          hadErrors = true;
          std::cerr << "[AUDIO] ALSA snd_ctl_pcm_next_device(" << name
                    << ") failed: " << snd_strerror(rc) << "\n";
        }
      } else {
        hadErrors = true;
        std::cerr << "[AUDIO] ALSA snd_ctl_card_info(" << name
                  << ") failed: " << snd_strerror(rc) << "\n";
      }
      snd_ctl_close(handle);
    } else {
      hadErrors = true;
      std::cerr << "[AUDIO] ALSA snd_ctl_open(" << name
                << ") failed: " << snd_strerror(rc) << "\n";
    }
    rc = snd_card_next(&card);
  }
  if (rc < 0) {
    hadErrors = true;
    std::cerr << "[AUDIO] ALSA snd_card_next failed: " << snd_strerror(rc)
              << "\n";
  }
  if (!listedAny) {
    std::cout << "  (no playback hardware devices found)\n";
  }
  return !hadErrors;
}

bool AudioOutput::initAlsa(const std::string &deviceName) {
  std::string alsaDevice = deviceName;
  int hwCard = -1, hwDev = -1;
  if (parseHwDeviceGlobal(deviceName, hwCard, hwDev)) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "plughw:%d,%d", hwCard, hwDev);
    alsaDevice = buf;
  } else if (deviceName.empty() || deviceName == "default") {
    alsaDevice = "default";
  } else if (deviceName.substr(0, 3) == "hw:") {
    alsaDevice = "plug" + deviceName;
  }

  if (m_verboseLogging) {
    std::cout << "[AUDIO] opening ALSA device: " << alsaDevice << "\n";
  }

  int err =
      snd_pcm_open(&m_alsaPcm, alsaDevice.c_str(), SND_PCM_STREAM_PLAYBACK, 0);
  if (err < 0) {
    std::cerr << "[AUDIO] ALSA snd_pcm_open failed: " << snd_strerror(err)
              << "\n";
    return false;
  }

  snd_pcm_hw_params_t *hwparams = nullptr;
  snd_pcm_hw_params_alloca(&hwparams);
  snd_pcm_hw_params_any(m_alsaPcm, hwparams);

  snd_pcm_hw_params_set_access(m_alsaPcm, hwparams,
                               SND_PCM_ACCESS_RW_INTERLEAVED);
  snd_pcm_hw_params_set_format(m_alsaPcm, hwparams, SND_PCM_FORMAT_S16_LE);
  snd_pcm_hw_params_set_channels(m_alsaPcm, hwparams, CHANNELS);

  unsigned int rate = SAMPLE_RATE;
  int dir = 0;
  err = snd_pcm_hw_params_set_rate_near(m_alsaPcm, hwparams, &rate, &dir);
  if (err < 0) {
    std::cerr << "[AUDIO] ALSA set_rate failed: " << snd_strerror(err) << "\n";
    snd_pcm_close(m_alsaPcm);
    m_alsaPcm = nullptr;
    return false;
  }
  if (rate != SAMPLE_RATE && m_verboseLogging) {
    std::cerr << "[AUDIO] ALSA rate " << SAMPLE_RATE << " not supported, using "
              << rate << " (will resample)\n";
  }

  // Lower-latency target: ~128 ms device buffer at 48 kHz.
  snd_pcm_uframes_t bufferSize = 4096;
  snd_pcm_hw_params_set_buffer_size_near(m_alsaPcm, hwparams, &bufferSize);

  // Lower-latency target: ~16 ms period at 48 kHz.
  snd_pcm_uframes_t periodSize = 512;
  snd_pcm_hw_params_set_period_size_near(m_alsaPcm, hwparams, &periodSize,
                                         &dir);

  err = snd_pcm_hw_params(m_alsaPcm, hwparams);
  if (err < 0) {
    std::cerr << "[AUDIO] ALSA snd_pcm_hw_params failed: " << snd_strerror(err)
              << "\n";
    snd_pcm_close(m_alsaPcm);
    m_alsaPcm = nullptr;
    return false;
  }

  snd_pcm_sw_params_t *swparams = nullptr;
  snd_pcm_sw_params_alloca(&swparams);
  snd_pcm_sw_params_current(m_alsaPcm, swparams);
  snd_pcm_sw_params_set_start_threshold(m_alsaPcm, swparams,
                                        bufferSize - periodSize);
  snd_pcm_sw_params_set_avail_min(m_alsaPcm, swparams, periodSize);
  snd_pcm_sw_params(m_alsaPcm, swparams);

  snd_pcm_get_params(m_alsaPcm, &bufferSize, &periodSize);

  if (m_verboseLogging) {
    std::cout << "[AUDIO] ALSA initialized: rate=" << rate
              << " buffer=" << bufferSize << " (" << (bufferSize * 1000 / rate)
              << "ms)"
              << " period=" << periodSize << "\n";
  }

  m_alsaThreadRunning = true;
  m_alsaOutputThread = std::thread(&AudioOutput::runAlsaOutputThread, this);
  return true;
}

void AudioOutput::runAlsaOutputThread() {
  snd_pcm_uframes_t bufferSize = 0;
  snd_pcm_uframes_t periodSize = 0;
  snd_pcm_get_params(m_alsaPcm, &bufferSize, &periodSize);

  const size_t kWriteFrames = static_cast<size_t>(periodSize);
  std::vector<int16_t> interleaved(kWriteFrames * CHANNELS, 0);

  for (size_t i = 0; i < 4; i++) {
    snd_pcm_writei(m_alsaPcm, interleaved.data(), kWriteFrames);
  }

  float lastL = 0.0f;
  float lastR = 0.0f;

  while (m_alsaThreadRunning.load()) {
    {
      std::unique_lock<std::mutex> lock(m_speakerMutex);
      m_speakerCv.wait_for(lock, std::chrono::milliseconds(50), [&]() {
        return !m_alsaThreadRunning.load() || m_speakerSize >= kWriteFrames * 2;
      });

      if (!m_alsaThreadRunning.load()) {
        break;
      }

      const size_t sampleCount = popSpeakerSamplesLocked(
          m_speakerScratch.data(), kWriteFrames * CHANNELS);
      const size_t samplePairs = sampleCount / 2;
      if (samplePairs > 0) {
        for (size_t i = 0; i < samplePairs; i++) {
          float l = m_speakerScratch[i * 2];
          float r = m_speakerScratch[i * 2 + 1];
          l = std::clamp(l, -1.0f, 1.0f);
          r = std::clamp(r, -1.0f, 1.0f);
          interleaved[i * 2] = static_cast<int16_t>(l * kInt16Max);
          interleaved[i * 2 + 1] = static_cast<int16_t>(r * kInt16Max);
          lastL = l;
          lastR = r;
        }
      }

      if (samplePairs < kWriteFrames) {
        const size_t missing = kWriteFrames - samplePairs;
        for (size_t i = 0; i < missing; i++) {
          interleaved[(samplePairs + i) * 2] = 0;
          interleaved[(samplePairs + i) * 2 + 1] = 0;
        }
      }
    }

    snd_pcm_sframes_t frames =
        snd_pcm_writei(m_alsaPcm, interleaved.data(), kWriteFrames);
    if (frames < 0) {
      if (frames == -EPIPE) {
        snd_pcm_prepare(m_alsaPcm);
        for (int i = 0; i < 2; i++) {
          snd_pcm_writei(m_alsaPcm, interleaved.data(), kWriteFrames);
        }
        if (m_verboseLogging) {
          static std::atomic<uint32_t> underflowCount{0};
          const uint32_t count = ++underflowCount;
          if (count <= 5 || (count % 50) == 0) {
            std::cerr << "[AUDIO] ALSA underrun (" << count << ")\n";
          }
        }
      } else if (frames == -EAGAIN) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      } else {
        if (m_verboseLogging) {
          std::cerr << "[AUDIO] ALSA write error: " << snd_strerror(frames)
                    << "\n";
        }
        snd_pcm_recover(m_alsaPcm, static_cast<int>(frames), 1);
      }
    }
  }
}

void AudioOutput::shutdownAlsa() {
  m_alsaThreadRunning = false;
  m_speakerCv.notify_all();
  if (m_alsaOutputThread.joinable()) {
    m_alsaOutputThread.join();
  }
  if (m_alsaPcm) {
    snd_pcm_drop(m_alsaPcm);
    snd_pcm_close(m_alsaPcm);
    m_alsaPcm = nullptr;
  }
}
#endif

#if defined(_WIN32) && defined(FM_TUNER_HAS_WINMM)
void AudioOutput::runWinMMOutputThread() {
  // Lower WinMM device buffering to reduce end-to-end output latency.
  constexpr size_t kFrames = 512;
  constexpr size_t kSamplesPerBuffer = kFrames * CHANNELS;
  constexpr size_t kNumBuffers = 4;

  std::vector<std::vector<int16_t>> buffers(
      kNumBuffers, std::vector<int16_t>(kSamplesPerBuffer, 0));
  std::vector<WAVEHDR> headers(kNumBuffers);
  for (size_t i = 0; i < kNumBuffers; i++) {
    std::memset(&headers[i], 0, sizeof(WAVEHDR));
    headers[i].lpData = reinterpret_cast<LPSTR>(buffers[i].data());
    headers[i].dwBufferLength =
        static_cast<DWORD>(kSamplesPerBuffer * sizeof(int16_t));
    const MMRESULT pr =
        waveOutPrepareHeader(m_waveOut, &headers[i], sizeof(WAVEHDR));
    if (pr != MMSYSERR_NOERROR) {
      if (m_verboseLogging) {
        std::cerr << "[AUDIO] WinMM waveOutPrepareHeader failed for buffer "
                  << i << ": " << pr << "\n";
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
    if (!hdr || !pcm) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      continue;
    }

    size_t copied = 0;
    {
      std::unique_lock<std::mutex> lock(m_speakerMutex);
      m_speakerCv.wait_for(lock, std::chrono::milliseconds(10), [&]() {
        return !m_winmmThreadRunning.load() || m_speakerSize > 0;
      });

      if (!m_winmmThreadRunning.load()) {
        break;
      }

      copied = popSpeakerSamplesLocked(m_speakerScratch.data(), kSamplesPerBuffer);
      for (size_t i = 0; i < copied; i++) {
        float v = m_speakerScratch[i];
        v = std::clamp(v, -1.0f, 1.0f);
        (*pcm)[i] = static_cast<int16_t>(v * 32767.0f);
      }
      for (size_t i = copied; i < kSamplesPerBuffer; i++) {
        (*pcm)[i] = 0;
      }
    }

    hdr->dwBufferLength =
        static_cast<DWORD>(kSamplesPerBuffer * sizeof(int16_t));
    const MMRESULT wr = waveOutWrite(m_waveOut, hdr, sizeof(WAVEHDR));
    if (wr != MMSYSERR_NOERROR && m_verboseLogging) {
      std::cerr << "[AUDIO] WinMM write failed: " << wr << "\n";
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }

  if (m_waveOut) {
    waveOutReset(m_waveOut);
  }
  for (size_t i = 0; i < kNumBuffers; i++) {
    waveOutUnprepareHeader(m_waveOut, &headers[i], sizeof(WAVEHDR));
  }
}
#endif

AudioOutput::AudioOutput()
    : m_enableSpeaker(false), m_wavHandle(nullptr), m_running(false),
      m_wavThreadRunning(false), m_wavFatalError(false), m_wavDataSize(0),
      m_verboseLogging(true),
      m_requestedVolumePercent(kMaxVolumePercent),
      m_currentVolumeScale(kDefaultVolumeScale), m_speakerReadPos(0),
      m_speakerWritePos(0), m_speakerSize(0), m_wavReadPos(0), m_wavWritePos(0),
      m_wavSize(0)
#if defined(__APPLE__) && defined(FM_TUNER_HAS_COREAUDIO)
      ,
      m_audioUnit(nullptr)
#endif
#if defined(_WIN32) && defined(FM_TUNER_HAS_WINMM)
      ,
      m_waveOut(nullptr), m_winmmThreadRunning(false)
#endif
#if defined(__linux__) && defined(FM_TUNER_HAS_ALSA)
      ,
      m_alsaPcm(nullptr), m_alsaThreadRunning(false)
#endif
{
  m_speakerRing.resize(kSpeakerQueueSamples, 0.0f);
  m_wavRing.resize(kWavQueueSamples, 0);
  m_speakerScratch.resize(FRAMES_PER_BUFFER * CHANNELS, 0.0f);
  m_wavEncodeScratch.resize(FRAMES_PER_BUFFER * CHANNELS, 0);
}

AudioOutput::~AudioOutput() { shutdown(); }

bool AudioOutput::init(bool enableSpeaker, const std::string &wavFile,
                       const std::string &deviceSelector, bool verboseLogging) {
  m_enableSpeaker = enableSpeaker;
  m_wavFile = wavFile;
  m_verboseLogging = verboseLogging;
  {
    std::lock_guard<std::mutex> lock(m_speakerMutex);
    clearSpeakerQueueLocked();
  }

  if (!wavFile.empty()) {
    if (!initWAV(wavFile)) {
      std::cerr << "Failed to initialize WAV file" << "\n";
      return false;
    }
  }

#if defined(__APPLE__) && defined(FM_TUNER_HAS_COREAUDIO)
  if (enableSpeaker) {
    const std::string normalizedSelector =
        normalizeCoreAudioSelector(deviceSelector);
    if (verboseLogging) {
      std::cout << "[AUDIO] coreaudio selector raw='" << deviceSelector
                << "' normalized='" << normalizedSelector << "'\n";
    }

    AudioComponentDescription desc{};
    desc.componentType = kAudioUnitType_Output;
    desc.componentSubType = kAudioUnitSubType_DefaultOutput;
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;
    AudioComponent component = AudioComponentFindNext(nullptr, &desc);
    if (!component) {
      std::cerr << "[AUDIO] CoreAudio output component not found\n";
      return false;
    }
    if (AudioComponentInstanceNew(component, &m_audioUnit) != noErr ||
        !m_audioUnit) {
      std::cerr << "[AUDIO] CoreAudio instance creation failed\n";
      m_audioUnit = nullptr;
      return false;
    }

    if (!normalizedSelector.empty()) {
      AudioDeviceID selected = selectOutputDeviceCoreAudio(normalizedSelector);
      if (selected == kAudioObjectUnknown) {
        std::cerr << "[AUDIO] CoreAudio device not found for selector: "
                  << normalizedSelector << "\n";
        listCoreAudioDevices();
        AudioComponentInstanceDispose(m_audioUnit);
        m_audioUnit = nullptr;
        return false;
      }
      const OSStatus selErr = AudioUnitSetProperty(
          m_audioUnit, kAudioOutputUnitProperty_CurrentDevice,
          kAudioUnitScope_Global, 0, &selected, sizeof(selected));
      if (selErr != noErr) {
        std::cerr << "[AUDIO] CoreAudio failed to select output device (status="
                  << selErr << ")\n";
        AudioComponentInstanceDispose(m_audioUnit);
        m_audioUnit = nullptr;
        return false;
      }
    }

    AURenderCallbackStruct callback{};
    callback.inputProc = &AudioOutput::coreAudioRenderCallback;
    callback.inputProcRefCon = this;
    if (AudioUnitSetProperty(m_audioUnit, kAudioUnitProperty_SetRenderCallback,
                             kAudioUnitScope_Input, 0, &callback,
                             sizeof(callback)) != noErr) {
      std::cerr << "[AUDIO] CoreAudio set render callback failed\n";
      AudioComponentInstanceDispose(m_audioUnit);
      m_audioUnit = nullptr;
      return false;
    }

    AudioStreamBasicDescription asbd{};
    asbd.mSampleRate = static_cast<Float64>(SAMPLE_RATE);
    asbd.mFormatID = kAudioFormatLinearPCM;
    asbd.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked |
                        kAudioFormatFlagIsNonInterleaved;
    asbd.mFramesPerPacket = 1;
    asbd.mChannelsPerFrame = CHANNELS;
    asbd.mBitsPerChannel = 32;
    asbd.mBytesPerFrame = sizeof(float);
    asbd.mBytesPerPacket = sizeof(float);
    if (AudioUnitSetProperty(m_audioUnit, kAudioUnitProperty_StreamFormat,
                             kAudioUnitScope_Input, 0, &asbd,
                             sizeof(asbd)) != noErr) {
      std::cerr << "[AUDIO] CoreAudio set stream format failed\n";
      AudioComponentInstanceDispose(m_audioUnit);
      m_audioUnit = nullptr;
      return false;
    }

    if (AudioUnitInitialize(m_audioUnit) != noErr) {
      std::cerr << "[AUDIO] CoreAudio initialize failed\n";
      AudioComponentInstanceDispose(m_audioUnit);
      m_audioUnit = nullptr;
      return false;
    }
    if (AudioOutputUnitStart(m_audioUnit) != noErr) {
      std::cerr << "[AUDIO] CoreAudio start failed\n";
      AudioUnitUninitialize(m_audioUnit);
      AudioComponentInstanceDispose(m_audioUnit);
      m_audioUnit = nullptr;
      return false;
    }
    if (verboseLogging) {
      std::cout << "[AUDIO] CoreAudio started successfully\n";
    }
  }
#endif

#if defined(_WIN32) && defined(FM_TUNER_HAS_WINMM)
  if (enableSpeaker) {
    const std::string normalizedSelector = trimWinMM(deviceSelector);
    if (verboseLogging) {
      std::cout << "[AUDIO] winmm selector raw='" << deviceSelector
                << "' normalized='" << normalizedSelector << "'\n";
    }
    const UINT devId = selectWinMMDevice(normalizedSelector);
    if (devId == UINT_MAX) {
      std::cerr << "WinMM device not found for selector: " << normalizedSelector
                << "\n";
      listWinMMDevices();
      return false;
    }

    WAVEFORMATEX fmt{};
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = static_cast<WORD>(CHANNELS);
    fmt.nSamplesPerSec = SAMPLE_RATE;
    fmt.wBitsPerSample = 16;
    fmt.nBlockAlign =
        static_cast<WORD>((fmt.nChannels * fmt.wBitsPerSample) / 8);
    fmt.nAvgBytesPerSec = fmt.nBlockAlign * fmt.nSamplesPerSec;
    fmt.cbSize = 0;

    const UINT openId = normalizedSelector.empty() ? WAVE_MAPPER : devId;
    const MMRESULT wr =
        waveOutOpen(&m_waveOut, openId, &fmt, 0, 0, CALLBACK_NULL);
    if (wr != MMSYSERR_NOERROR || !m_waveOut) {
      std::cerr << "WinMM open failed: " << wr << "\n";
      m_waveOut = nullptr;
      return false;
    }

    m_winmmThreadRunning = true;
    m_winmmThread = std::thread(&AudioOutput::runWinMMOutputThread, this);
    if (verboseLogging) {
      std::cout << "[AUDIO] WinMM started successfully\n";
    }
  }
#endif

#if defined(__linux__) && defined(FM_TUNER_HAS_ALSA)
  if (enableSpeaker) {
    std::string alsaDevice =
        deviceSelector.empty() ? "default" : deviceSelector;
    if (verboseLogging) {
      std::cout << "[AUDIO] device selector: " << alsaDevice << "\n";
    }
    if (!initAlsa(alsaDevice)) {
      std::cerr << "Failed to initialize ALSA audio output" << "\n";
      return false;
    }
  }
#endif

  m_running = true;
  return true;
}

void AudioOutput::shutdown() {
  m_running = false;

#if defined(__linux__) && defined(FM_TUNER_HAS_ALSA)
  shutdownAlsa();
#endif

#if defined(__APPLE__) && defined(FM_TUNER_HAS_COREAUDIO)
  if (m_audioUnit) {
    AudioOutputUnitStop(m_audioUnit);
    AudioUnitUninitialize(m_audioUnit);
    AudioComponentInstanceDispose(m_audioUnit);
    m_audioUnit = nullptr;
  }
#endif

#if defined(_WIN32) && defined(FM_TUNER_HAS_WINMM)
  m_winmmThreadRunning = false;
  m_speakerCv.notify_all();
  if (m_winmmThread.joinable()) {
    m_winmmThread.join();
  }
  if (m_waveOut) {
    waveOutClose(m_waveOut);
    m_waveOut = nullptr;
  }
#endif

  closeWAV();
}

void AudioOutput::setVolumePercent(int volumePercent) {
  m_requestedVolumePercent.store(
      std::clamp(volumePercent, 0, kMaxVolumePercent),
      std::memory_order_relaxed);
}

bool AudioOutput::initWAV(const std::string &filename) {
  m_wavHandle = fopen(filename.c_str(), "wb");
  if (!m_wavHandle) {
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(m_wavMutex);
    m_wavDataSize = 0;
    m_wavReadPos = 0;
    m_wavWritePos = 0;
    m_wavSize = 0;
  }
  m_wavFatalError.store(false);
  if (!writeWAVHeader()) {
    fclose(m_wavHandle);
    m_wavHandle = nullptr;
    return false;
  }
  m_wavThreadRunning = true;
  m_wavThread = std::thread(&AudioOutput::runWavWriterThread, this);
  return true;
}

bool AudioOutput::writeWAVHeader() {
  if (!m_wavHandle)
    return false;

  const uint32_t sampleRate = SAMPLE_RATE;
  const uint16_t numChannels = CHANNELS;
  const uint16_t bitsPerSample = BITS_PER_SAMPLE;
  const uint32_t byteRate = sampleRate * numChannels * bitsPerSample / 8;
  const uint16_t blockAlign = numChannels * bitsPerSample / 8;
  const uint32_t dataSize = m_wavDataSize;
  const uint32_t fileSize = 36u + dataSize;
  const uint32_t fmtSize = 16;
  const uint16_t audioFormat = 1;

  if (fseek(m_wavHandle, 0, SEEK_SET) != 0) {
    if (m_verboseLogging) {
      std::cerr << "[AUDIO] failed to seek WAV header\n";
    }
    return false;
  }

  const bool ok =
      fwrite("RIFF", 1, 4, m_wavHandle) == 4 &&
      fwrite(&fileSize, 4, 1, m_wavHandle) == 1 &&
      fwrite("WAVE", 1, 4, m_wavHandle) == 4 &&
      fwrite("fmt ", 1, 4, m_wavHandle) == 4 &&
      fwrite(&fmtSize, 4, 1, m_wavHandle) == 1 &&
      fwrite(&audioFormat, 2, 1, m_wavHandle) == 1 &&
      fwrite(&numChannels, 2, 1, m_wavHandle) == 1 &&
      fwrite(&sampleRate, 4, 1, m_wavHandle) == 1 &&
      fwrite(&byteRate, 4, 1, m_wavHandle) == 1 &&
      fwrite(&blockAlign, 2, 1, m_wavHandle) == 1 &&
      fwrite(&bitsPerSample, 2, 1, m_wavHandle) == 1 &&
      fwrite("data", 1, 4, m_wavHandle) == 4 &&
      fwrite(&dataSize, 4, 1, m_wavHandle) == 1;
  if (!ok && m_verboseLogging) {
    std::cerr << "[AUDIO] failed to write WAV header\n";
  }
  return ok;
}

bool AudioOutput::writeWAVData(const int16_t *samples, size_t sampleCount) {
  if (!m_wavHandle || !samples || sampleCount == 0)
    return false;
  if (m_wavFatalError.load())
    return false;

  const size_t maxBytes =
      static_cast<size_t>(UINT32_MAX) - 36u - m_wavDataSize;
  const size_t wantBytes = sampleCount * sizeof(int16_t);
  if (wantBytes > maxBytes) {
    if (m_verboseLogging) {
      std::cerr << "[AUDIO] reached 4 GiB WAV limit, stopping capture\n";
    }
    m_wavFatalError.store(true);
    return false;
  }

  const size_t written =
      fwrite(samples, sizeof(int16_t), sampleCount, m_wavHandle);
  m_wavDataSize += static_cast<uint32_t>(written * sizeof(int16_t));
  if (written != sampleCount) {
    if (m_verboseLogging) {
      std::cerr << "[AUDIO] short WAV write (" << written << "/" << sampleCount
                << ")\n";
    }
    m_wavFatalError.store(true);
    return false;
  }
  return true;
}

bool AudioOutput::enqueueWavSamples(const float *left, const float *right,
                                    size_t numSamples) {
  if (!m_wavHandle || !left || !right || numSamples == 0 || m_wavRing.empty()) {
    return false;
  }
  const size_t sampleCount = numSamples * CHANNELS;
  if (m_wavEncodeScratch.size() < sampleCount) {
    m_wavEncodeScratch.resize(sampleCount);
  }
  for (size_t i = 0; i < numSamples; i++) {
    const float l = std::clamp(left[i], -1.0f, 1.0f);
    const float r = std::clamp(right[i], -1.0f, 1.0f);
    m_wavEncodeScratch[i * 2] = static_cast<int16_t>(l * kInt16Max);
    m_wavEncodeScratch[i * 2 + 1] = static_cast<int16_t>(r * kInt16Max);
  }

  std::lock_guard<std::mutex> lock(m_wavMutex);
  const size_t capacity = m_wavRing.size();
  size_t start = 0;
  if (sampleCount >= capacity) {
    start = sampleCount - capacity;
  }
  const size_t kept = sampleCount - start;
  if (m_wavSize + kept > capacity) {
    size_t drop = m_wavSize + kept - capacity;
    drop = std::min(drop, m_wavSize);
    m_wavReadPos = (m_wavReadPos + drop) % capacity;
    m_wavSize -= drop;
    if (m_verboseLogging) {
      static std::atomic<uint32_t> overflowCount{0};
      const uint32_t count = ++overflowCount;
      if (count <= 5 || (count % 50) == 0) {
        std::cerr << "[AUDIO] WAV queue overflow (" << count << ")\n";
      }
    }
  }
  for (size_t i = start; i < sampleCount; i++) {
    m_wavRing[m_wavWritePos] = m_wavEncodeScratch[i];
    m_wavWritePos = (m_wavWritePos + 1) % capacity;
  }
  m_wavSize += kept;
  m_wavCv.notify_one();
  return true;
}

void AudioOutput::runWavWriterThread() {
  std::vector<int16_t> localBuffer(FRAMES_PER_BUFFER * CHANNELS, 0);
  while (true) {
    size_t copied = 0;
    {
      std::unique_lock<std::mutex> lock(m_wavMutex);
      m_wavCv.wait_for(lock, std::chrono::milliseconds(50), [&]() {
        return !m_wavThreadRunning.load() || m_wavSize > 0;
      });
      if (!m_wavThreadRunning.load() && m_wavSize == 0) {
        break;
      }
      if (m_wavSize == 0) {
        continue;
      }
      copied = std::min(localBuffer.size(), m_wavSize);
      for (size_t i = 0; i < copied; i++) {
        localBuffer[i] = m_wavRing[m_wavReadPos];
        m_wavReadPos = (m_wavReadPos + 1) % m_wavRing.size();
      }
      m_wavSize -= copied;
    }
    if (!writeWAVData(localBuffer.data(), copied)) {
      break;
    }
  }
}

void AudioOutput::closeWAV() {
  if (m_wavHandle) {
    m_wavThreadRunning = false;
    m_wavCv.notify_all();
    if (m_wavThread.joinable()) {
      m_wavThread.join();
    }
    if (!writeWAVHeader() && !m_wavFatalError.load() && m_verboseLogging) {
      std::cerr << "[AUDIO] final WAV header write failed\n";
    }
    fclose(m_wavHandle);
    m_wavHandle = nullptr;
  }
}

void AudioOutput::clearRealtimeQueue() {
  std::lock_guard<std::mutex> lock(m_speakerMutex);
  clearSpeakerQueueLocked();
}

bool AudioOutput::write(const float *left, const float *right,
                        size_t numSamples) {
  if (!m_running)
    return false;

  const float *writeLeft = left;
  const float *writeRight = right;

  if (left && right && numSamples > 0) {
    if (m_scaledLeftScratch.size() < numSamples) {
      m_scaledLeftScratch.resize(numSamples);
    }
    if (m_scaledRightScratch.size() < numSamples) {
      m_scaledRightScratch.resize(numSamples);
    }
    const float targetVolumeScale =
        (static_cast<float>(
             m_requestedVolumePercent.load(std::memory_order_relaxed)) /
         static_cast<float>(kMaxVolumePercent)) *
        kDefaultVolumeScale;
    const float rampSamples = static_cast<float>(SAMPLE_RATE) * 0.01f;
    const float step = (targetVolumeScale - m_currentVolumeScale) /
                       std::max(1.0f, rampSamples);

    for (size_t i = 0; i < numSamples; i++) {
      if (std::abs(targetVolumeScale - m_currentVolumeScale) > kVolumeEpsilon) {
        m_currentVolumeScale += step;
        if ((step > 0.0f && m_currentVolumeScale > targetVolumeScale) ||
            (step < 0.0f && m_currentVolumeScale < targetVolumeScale)) {
          m_currentVolumeScale = targetVolumeScale;
        }
      }
      m_scaledLeftScratch[i] = left[i] * m_currentVolumeScale;
      m_scaledRightScratch[i] = right[i] * m_currentVolumeScale;
    }
    writeLeft = m_scaledLeftScratch.data();
    writeRight = m_scaledRightScratch.data();
  }

  if (m_wavHandle) {
    (void)enqueueWavSamples(writeLeft, writeRight, numSamples);
  }

#if defined(__linux__) && defined(FM_TUNER_HAS_ALSA)
  if (m_enableSpeaker && m_alsaPcm) {
    pushSpeakerSamples(writeLeft, writeRight, numSamples, "ALSA");
  }
#endif

#if defined(__APPLE__) && defined(FM_TUNER_HAS_COREAUDIO)
  if (m_enableSpeaker && m_audioUnit) {
    pushSpeakerSamples(writeLeft, writeRight, numSamples, "CoreAudio");
  }
#endif
#if defined(_WIN32) && defined(FM_TUNER_HAS_WINMM)
  if (m_enableSpeaker && m_waveOut) {
    pushSpeakerSamples(writeLeft, writeRight, numSamples, "WinMM");
  }
#endif

  return true;
}
