#include "audio_output.h"
#include <iostream>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <cctype>
#include <chrono>
#if defined(__APPLE__) && defined(FM_TUNER_HAS_COREAUDIO)
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#endif

#if defined(__linux__) && defined(FM_TUNER_HAS_ALSA)
namespace {
bool parseHwDeviceGlobal(const std::string& selector, int& card, int& device) {
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
    for (char c : cardStr) { if (c < '0' || c > '9') return false; }
    for (char c : devStr) { if (c < '0' || c > '9') return false; }
    card = std::stoi(cardStr);
    device = std::stoi(devStr);
    return true;
}
}
#endif

#if defined(FM_TUNER_HAS_PORTAUDIO)
namespace {
std::string trim(const std::string& value) {
    size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start])) != 0) {
        start++;
    }
    size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        end--;
    }
    return value.substr(start, end - start);
}

std::string normalizeSelector(const std::string& rawSelector) {
    std::string selector = trim(rawSelector);
    if (selector.size() >= 2) {
        const char first = selector.front();
        const char last = selector.back();
        if ((first == '"' && last == '"') || (first == '\'' && last == '\'')) {
            selector = trim(selector.substr(1, selector.size() - 2));
        }
    }
    return selector;
}

std::string toLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool parseDeviceIndex(const std::string& selector, int& outIndex) {
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

bool parseHwDevice(const std::string& selector, int& card, int& device) {
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
    for (char c : cardStr) { if (c < '0' || c > '9') return false; }
    for (char c : devStr) { if (c < '0' || c > '9') return false; }
    card = std::stoi(cardStr);
    device = std::stoi(devStr);
    return true;
}

PaDeviceIndex selectOutputDevice(const std::string& selector) {
    if (selector.empty()) {
        return Pa_GetDefaultOutputDevice();
    }

    int requestedIndex = -1;
    if (parseDeviceIndex(selector, requestedIndex)) {
        const int deviceCount = Pa_GetDeviceCount();
        if (requestedIndex >= 0 && requestedIndex < deviceCount) {
            const PaDeviceInfo* info = Pa_GetDeviceInfo(requestedIndex);
            if (info && info->maxOutputChannels > 0) {
                return static_cast<PaDeviceIndex>(requestedIndex);
            }
        }
        return paNoDevice;
    }

    int hwCard = -1, hwDev = -1;
    if (parseHwDevice(selector, hwCard, hwDev)) {
        char hwPattern[32];
        std::snprintf(hwPattern, sizeof(hwPattern), "(hw:%d,%d)", hwCard, hwDev);
        const int deviceCount = Pa_GetDeviceCount();
        for (int i = 0; i < deviceCount; i++) {
            const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
            if (!info || info->maxOutputChannels <= 0 || !info->name) {
                continue;
            }
            if (std::strstr(info->name, hwPattern) != nullptr) {
                return static_cast<PaDeviceIndex>(i);
            }
        }
        return paNoDevice;
    }

    const std::string needle = toLower(selector);
    const int deviceCount = Pa_GetDeviceCount();
    for (int i = 0; i < deviceCount; i++) {
        const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
        if (!info || info->maxOutputChannels <= 0 || !info->name) {
            continue;
        }
        if (toLower(info->name).find(needle) != std::string::npos) {
            return static_cast<PaDeviceIndex>(i);
        }
    }

    return paNoDevice;
}

void printOutputDeviceList() {
    const int deviceCount = Pa_GetDeviceCount();
    std::cout << "Available audio output devices:\n";
    for (int i = 0; i < deviceCount; i++) {
        const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
        if (!info || info->maxOutputChannels <= 0 || !info->name) {
            continue;
        }
        const char* name = info->name;
        if (std::strstr(name, "lavrate") != nullptr ||
            std::strstr(name, "samplerate") != nullptr ||
            std::strstr(name, "speexrate") != nullptr ||
            std::strstr(name, "upmix") != nullptr ||
            std::strstr(name, "vdownmix") != nullptr ||
            std::strstr(name, "dmix") != nullptr ||
            std::strstr(name, "oss") != nullptr ||
            std::strstr(name, "pipewire") != nullptr ||
            std::strstr(name, "pulse") != nullptr) {
            continue;
        }
        std::cout << "  [" << i << "] " << name;
        const PaHostApiInfo* api = Pa_GetHostApiInfo(info->hostApi);
        if (api) {
            std::cout << " (API: " << api->name << ")";
        }
        std::cout << "\n";
    }
}
}  // namespace

#endif

#if defined(__APPLE__) && defined(FM_TUNER_HAS_COREAUDIO)
namespace {
std::string trimCoreAudio(const std::string& value) {
    size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start])) != 0) {
        start++;
    }
    size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        end--;
    }
    return value.substr(start, end - start);
}

std::string normalizeCoreAudioSelector(const std::string& rawSelector) {
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

bool parseCoreAudioDeviceIndex(const std::string& selector, int& outIndex) {
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
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string cfStringToStdString(CFStringRef str) {
    if (!str) {
        return std::string();
    }
    const CFIndex length = CFStringGetLength(str);
    const CFIndex maxSize = CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8) + 1;
    std::string result(static_cast<size_t>(maxSize), '\0');
    if (CFStringGetCString(str, result.data(), maxSize, kCFStringEncodingUTF8) == 0) {
        return std::string();
    }
    result.resize(std::strlen(result.c_str()));
    return result;
}

bool deviceHasOutputChannels(AudioDeviceID deviceId) {
    AudioObjectPropertyAddress addr{
        kAudioDevicePropertyStreamConfiguration,
        kAudioDevicePropertyScopeOutput,
        kAudioObjectPropertyElementWildcard
    };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(deviceId, &addr, 0, nullptr, &size) != noErr || size == 0) {
        return false;
    }
    std::vector<uint8_t> storage(size);
    AudioBufferList* bufferList = reinterpret_cast<AudioBufferList*>(storage.data());
    if (AudioObjectGetPropertyData(deviceId, &addr, 0, nullptr, &size, bufferList) != noErr) {
        return false;
    }
    UInt32 channels = 0;
    for (UInt32 i = 0; i < bufferList->mNumberBuffers; i++) {
        channels += bufferList->mBuffers[i].mNumberChannels;
    }
    return channels > 0;
}

std::vector<AudioDeviceID> enumerateOutputDevices() {
    AudioObjectPropertyAddress addr{
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &addr, 0, nullptr, &size) != noErr || size == 0) {
        return {};
    }
    std::vector<AudioDeviceID> devices(size / sizeof(AudioDeviceID));
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr, &size, devices.data()) != noErr) {
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
    AudioObjectPropertyAddress addr{
        kAudioHardwarePropertyDefaultOutputDevice,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr, &size, &device) != noErr) {
        return kAudioObjectUnknown;
    }
    return device;
}

std::string outputDeviceName(AudioDeviceID deviceId) {
    CFStringRef cfName = nullptr;
    UInt32 size = sizeof(cfName);
    AudioObjectPropertyAddress addr{
        kAudioObjectPropertyName,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    if (AudioObjectGetPropertyData(deviceId, &addr, 0, nullptr, &size, &cfName) != noErr || !cfName) {
        return std::string();
    }
    std::string name = cfStringToStdString(cfName);
    CFRelease(cfName);
    return name;
}

AudioDeviceID selectOutputDeviceCoreAudio(const std::string& selector) {
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
        if (requestedIndex >= 0 && requestedIndex < static_cast<int>(devices.size())) {
            return devices[static_cast<size_t>(requestedIndex)];
        }
        return kAudioObjectUnknown;
    }
    const std::string needle = toLowerCoreAudio(selector);
    for (AudioDeviceID id : devices) {
        const std::string name = outputDeviceName(id);
        if (!name.empty() && toLowerCoreAudio(name).find(needle) != std::string::npos) {
            return id;
        }
    }
    return kAudioObjectUnknown;
}
}  // namespace
#endif

#if defined(_WIN32) && defined(FM_TUNER_HAS_WINMM)
namespace {
std::string narrowFromWide(const WCHAR* wide) {
    if (!wide || *wide == L'\0') {
        return std::string();
    }
    const int len = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) {
        return std::string();
    }
    std::string out(static_cast<size_t>(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), len, nullptr, nullptr);
    return out;
}

std::string trimWinMM(const std::string& value) {
    size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start])) != 0) {
        start++;
    }
    size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        end--;
    }
    return value.substr(start, end - start);
}

bool parseWinMMIndex(const std::string& selector, UINT& index) {
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
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

UINT selectWinMMDevice(const std::string& selector) {
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
}  // namespace
#endif

bool AudioOutput::listDevices() {
#if defined(__linux__) && defined(FM_TUNER_HAS_ALSA)
    listAlsaDevices();
#elif defined(__APPLE__) && defined(FM_TUNER_HAS_COREAUDIO)
    return listCoreAudioDevices();
#elif defined(_WIN32) && defined(FM_TUNER_HAS_WINMM)
    return listWinMMDevices();
#elif defined(FM_TUNER_HAS_PORTAUDIO)
    PaError err = Pa_Initialize();
    if (err != paNoError) {
        std::cerr << "PortAudio init failed: " << Pa_GetErrorText(err) << std::endl;
        return false;
    }
    printOutputDeviceList();
    Pa_Terminate();
#else
    std::cerr << "Audio device listing is unavailable in this build "
                 "(enable PortAudio on macOS/Windows or ALSA on Linux)\n";
    return false;
#endif
    return true;
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
        std::cout << "  [" << i << "] " << narrowFromWide(caps.szPname) << " (API: WinMM)\n";
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
        std::cout << "  [" << i << "] " << (name.empty() ? "(unknown)" : name) << " (API: Core Audio)\n";
    }
    return true;
}

OSStatus AudioOutput::coreAudioRenderCallback(void* inRefCon,
                                              AudioUnitRenderActionFlags*,
                                              const AudioTimeStamp*,
                                              UInt32,
                                              UInt32 inNumberFrames,
                                              AudioBufferList* ioData) {
    auto* self = reinterpret_cast<AudioOutput*>(inRefCon);
    if (!self || !ioData || inNumberFrames == 0) {
        return noErr;
    }

    float* outA = (ioData->mNumberBuffers > 0)
        ? reinterpret_cast<float*>(ioData->mBuffers[0].mData)
        : nullptr;
    float* outB = (ioData->mNumberBuffers > 1)
        ? reinterpret_cast<float*>(ioData->mBuffers[1].mData)
        : nullptr;
    if (!outA) {
        return noErr;
    }

    std::lock_guard<std::mutex> lock(self->m_outputMutex);
    const size_t available = (self->m_outputQueue.size() > self->m_outputReadIndex)
        ? (self->m_outputQueue.size() - self->m_outputReadIndex)
        : 0;
    const size_t samplePairs = std::min(available / 2, static_cast<size_t>(inNumberFrames));
    for (size_t i = 0; i < samplePairs; i++) {
        const float l = self->m_outputQueue[self->m_outputReadIndex + i * 2];
        const float r = self->m_outputQueue[self->m_outputReadIndex + i * 2 + 1];
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

    self->m_outputReadIndex += samplePairs * 2;
    if (self->m_outputReadIndex > 0 &&
        (self->m_outputReadIndex >= 16384 || (self->m_outputReadIndex * 2) >= self->m_outputQueue.size())) {
        self->m_outputQueue.erase(self->m_outputQueue.begin(),
                                  self->m_outputQueue.begin() + static_cast<std::ptrdiff_t>(self->m_outputReadIndex));
        self->m_outputReadIndex = 0;
    }
    return noErr;
}
#endif

#if defined(__linux__) && defined(FM_TUNER_HAS_ALSA)
bool AudioOutput::listAlsaDevices() {
    std::cout << "ALSA hardware devices:\n";
    int card = -1;
    while (snd_card_next(&card) >= 0 && card >= 0) {
        char name[32];
        std::snprintf(name, sizeof(name), "hw:%d", card);
        snd_ctl_t* handle = nullptr;
        if (snd_ctl_open(&handle, name, 0) >= 0) {
            snd_ctl_card_info_t* info = nullptr;
            snd_ctl_card_info_alloca(&info);
            if (snd_ctl_card_info(handle, info) >= 0) {
                const char* cardName = snd_ctl_card_info_get_name(info);
                int device = -1;
                while (snd_ctl_pcm_next_device(handle, &device) >= 0 && device >= 0) {
                    snd_pcm_info_t* pcminfo = nullptr;
                    snd_pcm_info_alloca(&pcminfo);
                    snd_pcm_info_set_device(pcminfo, device);
                    snd_pcm_info_set_subdevice(pcminfo, 0);
                    snd_pcm_info_set_stream(pcminfo, SND_PCM_STREAM_PLAYBACK);
                    if (snd_ctl_pcm_info(handle, pcminfo) >= 0) {
                        const char* devName = snd_pcm_info_get_name(pcminfo);
                        std::cout << "  [hw:" << card << "," << device << "] " << cardName << ": " << devName << "\n";
                    }
                }
            }
            snd_ctl_close(handle);
        }
    }
    return true;
}

bool AudioOutput::initAlsa(const std::string& deviceName) {
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

    int err = snd_pcm_open(&m_alsaPcm, alsaDevice.c_str(), SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) {
        std::cerr << "[AUDIO] ALSA snd_pcm_open failed: " << snd_strerror(err) << "\n";
        return false;
    }

    snd_pcm_hw_params_t* hwparams = nullptr;
    snd_pcm_hw_params_alloca(&hwparams);
    snd_pcm_hw_params_any(m_alsaPcm, hwparams);

    snd_pcm_hw_params_set_access(m_alsaPcm, hwparams, SND_PCM_ACCESS_RW_INTERLEAVED);
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
        std::cerr << "[AUDIO] ALSA rate " << SAMPLE_RATE << " not supported, using " << rate << " (will resample)\n";
    }

    snd_pcm_uframes_t bufferSize = 16384;
    snd_pcm_hw_params_set_buffer_size_near(m_alsaPcm, hwparams, &bufferSize);

    snd_pcm_uframes_t periodSize = 2048;
    snd_pcm_hw_params_set_period_size_near(m_alsaPcm, hwparams, &periodSize, &dir);

    err = snd_pcm_hw_params(m_alsaPcm, hwparams);
    if (err < 0) {
        std::cerr << "[AUDIO] ALSA snd_pcm_hw_params failed: " << snd_strerror(err) << "\n";
        snd_pcm_close(m_alsaPcm);
        m_alsaPcm = nullptr;
        return false;
    }

    snd_pcm_sw_params_t* swparams = nullptr;
    snd_pcm_sw_params_alloca(&swparams);
    snd_pcm_sw_params_current(m_alsaPcm, swparams);
    snd_pcm_sw_params_set_start_threshold(m_alsaPcm, swparams, bufferSize - periodSize);
    snd_pcm_sw_params_set_avail_min(m_alsaPcm, swparams, periodSize);
    snd_pcm_sw_params(m_alsaPcm, swparams);

    snd_pcm_get_params(m_alsaPcm, &bufferSize, &periodSize);

    if (m_verboseLogging) {
        std::cout << "[AUDIO] ALSA initialized: rate=" << rate 
                  << " buffer=" << bufferSize << " (" << (bufferSize * 1000 / rate) << "ms)"
                  << " period=" << periodSize << "\n";
    }

    m_alsaBuffer.reserve(static_cast<size_t>(rate) * 2);
    m_alsaReadIndex = 0;
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
        size_t samplePairs = 0;
        {
            std::unique_lock<std::mutex> lock(m_alsaMutex);
            m_alsaCv.wait_for(lock, std::chrono::milliseconds(50), [&]() {
                const size_t available = (m_alsaBuffer.size() > m_alsaReadIndex)
                    ? (m_alsaBuffer.size() - m_alsaReadIndex)
                    : 0;
                return !m_alsaThreadRunning.load() || available >= kWriteFrames * 2;
            });

            if (!m_alsaThreadRunning.load()) {
                break;
            }

            const size_t available = (m_alsaBuffer.size() > m_alsaReadIndex)
                ? (m_alsaBuffer.size() - m_alsaReadIndex)
                : 0;
            samplePairs = std::min(available / 2, kWriteFrames);
            
            if (samplePairs > 0) {
                for (size_t i = 0; i < samplePairs; i++) {
                    float l = m_alsaBuffer[m_alsaReadIndex + i * 2];
                    float r = m_alsaBuffer[m_alsaReadIndex + i * 2 + 1];
                    l = std::clamp(l, -1.0f, 1.0f);
                    r = std::clamp(r, -1.0f, 1.0f);
                    interleaved[i * 2] = static_cast<int16_t>(l * 32767.0f);
                    interleaved[i * 2 + 1] = static_cast<int16_t>(r * 32767.0f);
                    lastL = l;
                    lastR = r;
                }
                m_alsaReadIndex += samplePairs * 2;
            }

            if (samplePairs < kWriteFrames) {
                const size_t missing = kWriteFrames - samplePairs;
                for (size_t i = 0; i < missing; i++) {
                    interleaved[(samplePairs + i) * 2] = 0;
                    interleaved[(samplePairs + i) * 2 + 1] = 0;
                }
            }

            const size_t cleanupThreshold = std::max<size_t>(32768, kWriteFrames * 8);
            if (m_alsaReadIndex > 0 && (m_alsaReadIndex >= cleanupThreshold || (m_alsaReadIndex * 2) >= m_alsaBuffer.size())) {
                if (m_alsaReadIndex < m_alsaBuffer.size()) {
                    m_alsaBuffer.erase(m_alsaBuffer.begin(), m_alsaBuffer.begin() + static_cast<std::ptrdiff_t>(m_alsaReadIndex));
                } else {
                    m_alsaBuffer.clear();
                }
                m_alsaReadIndex = 0;
            }
        }

        snd_pcm_sframes_t frames = snd_pcm_writei(m_alsaPcm, interleaved.data(), kWriteFrames);
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
                    std::cerr << "[AUDIO] ALSA write error: " << snd_strerror(frames) << "\n";
                }
                snd_pcm_recover(m_alsaPcm, static_cast<int>(frames), 1);
            }
        }
    }
}

void AudioOutput::shutdownAlsa() {
    m_alsaThreadRunning = false;
    m_alsaCv.notify_all();
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

#if defined(FM_TUNER_HAS_PORTAUDIO)
void AudioOutput::runOutputThread() {
    constexpr size_t kBlockSamples = static_cast<size_t>(FRAMES_PER_BUFFER) * CHANNELS;
    std::vector<float> block(kBlockSamples, 0.0f);
    float lastL = 0.0f;
    float lastR = 0.0f;

    while (m_outputThreadRunning.load()) {
        size_t copied = 0;
        {
            std::unique_lock<std::mutex> lock(m_outputMutex);
            m_outputCv.wait_for(lock, std::chrono::milliseconds(10), [&]() {
                const size_t available = (m_outputQueue.size() > m_outputReadIndex)
                    ? (m_outputQueue.size() - m_outputReadIndex)
                    : 0;
                return !m_outputThreadRunning.load() || available > 0;
            });

            if (!m_outputThreadRunning.load()) {
                break;
            }

            const size_t available = (m_outputQueue.size() > m_outputReadIndex)
                ? (m_outputQueue.size() - m_outputReadIndex)
                : 0;
            copied = std::min(available, kBlockSamples);
            if (copied > 0) {
                std::memcpy(block.data(), m_outputQueue.data() + m_outputReadIndex, copied * sizeof(float));
                m_outputReadIndex += copied;
            }

            if (copied >= 2) {
                lastL = block[copied - 2];
                lastR = block[copied - 1];
            }

            if (copied == 0) {
                std::fill(block.begin(), block.end(), 0.0f);
                lastL = 0.0f;
                lastR = 0.0f;
            } else if (copied < kBlockSamples) {
                const size_t missing = kBlockSamples - copied;
                constexpr int kFadeMs = 5;
                const size_t fadeSamples = std::min(
                    missing,
                    static_cast<size_t>(SAMPLE_RATE * (static_cast<float>(kFadeMs) / 1000.0f) * CHANNELS));
                for (size_t i = 0; i < fadeSamples; i += 2) {
                    const float t = static_cast<float>(i / 2) /
                        static_cast<float>(std::max<size_t>(1, (fadeSamples / 2) - 1));
                    const float gain = 1.0f - t;
                    const size_t idx = copied + i;
                    block[idx] = lastL * gain;
                    if (idx + 1 < kBlockSamples) {
                        block[idx + 1] = lastR * gain;
                    }
                }
                for (size_t i = copied + fadeSamples; i < kBlockSamples; i++) {
                    block[i] = 0.0f;
                }
            }

            if (m_outputReadIndex > 0 && (m_outputReadIndex >= 16384 || (m_outputReadIndex * 2) >= m_outputQueue.size())) {
                m_outputQueue.erase(m_outputQueue.begin(), m_outputQueue.begin() + static_cast<std::ptrdiff_t>(m_outputReadIndex));
                m_outputReadIndex = 0;
            }
        }

        const PaError writeErr = Pa_WriteStream(m_paStream, block.data(), FRAMES_PER_BUFFER);
        if (writeErr == paOutputUnderflowed) {
            static std::atomic<uint32_t> underflowCount{0};
            const uint32_t count = ++underflowCount;
            if (m_verboseLogging && (count <= 5 || (count % 50) == 0)) {
                std::cerr << "[AUDIO] output underflow (" << count
                          << ") - consider higher system audio buffer/less CPU load\n";
            }
        } else if (writeErr != paNoError) {
            if (m_verboseLogging) {
                std::cerr << "[AUDIO] write failed: " << Pa_GetErrorText(writeErr) << "\n";
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
}
#endif

#if defined(_WIN32) && defined(FM_TUNER_HAS_WINMM)
void AudioOutput::runWinMMOutputThread() {
    constexpr size_t kFrames = static_cast<size_t>(FRAMES_PER_BUFFER);
    constexpr size_t kSamplesPerBuffer = kFrames * CHANNELS;
    constexpr size_t kNumBuffers = 4;

    std::vector<std::vector<int16_t>> buffers(kNumBuffers, std::vector<int16_t>(kSamplesPerBuffer, 0));
    std::vector<WAVEHDR> headers(kNumBuffers);
    for (size_t i = 0; i < kNumBuffers; i++) {
        std::memset(&headers[i], 0, sizeof(WAVEHDR));
        headers[i].lpData = reinterpret_cast<LPSTR>(buffers[i].data());
        headers[i].dwBufferLength = static_cast<DWORD>(kSamplesPerBuffer * sizeof(int16_t));
        headers[i].dwFlags = WHDR_DONE;
        waveOutPrepareHeader(m_waveOut, &headers[i], sizeof(WAVEHDR));
    }

    while (m_winmmThreadRunning.load()) {
        WAVEHDR* hdr = nullptr;
        std::vector<int16_t>* pcm = nullptr;
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
            std::unique_lock<std::mutex> lock(m_outputMutex);
            m_outputCv.wait_for(lock, std::chrono::milliseconds(10), [&]() {
                const size_t available = (m_outputQueue.size() > m_outputReadIndex)
                    ? (m_outputQueue.size() - m_outputReadIndex)
                    : 0;
                return !m_winmmThreadRunning.load() || available > 0;
            });

            if (!m_winmmThreadRunning.load()) {
                break;
            }

            const size_t available = (m_outputQueue.size() > m_outputReadIndex)
                ? (m_outputQueue.size() - m_outputReadIndex)
                : 0;
            copied = std::min(available, kSamplesPerBuffer);
            for (size_t i = 0; i < copied; i++) {
                float v = m_outputQueue[m_outputReadIndex + i];
                v = std::clamp(v, -1.0f, 1.0f);
                (*pcm)[i] = static_cast<int16_t>(v * 32767.0f);
            }
            for (size_t i = copied; i < kSamplesPerBuffer; i++) {
                (*pcm)[i] = 0;
            }
            m_outputReadIndex += copied;
            if (m_outputReadIndex > 0 &&
                (m_outputReadIndex >= 16384 || (m_outputReadIndex * 2) >= m_outputQueue.size())) {
                m_outputQueue.erase(m_outputQueue.begin(),
                                    m_outputQueue.begin() + static_cast<std::ptrdiff_t>(m_outputReadIndex));
                m_outputReadIndex = 0;
            }
        }

        hdr->dwBufferLength = static_cast<DWORD>(kSamplesPerBuffer * sizeof(int16_t));
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
    : m_enableSpeaker(false)
    , m_wavHandle(nullptr)
    , m_running(false)
    , m_wavDataSize(0)
    , m_writeIndex(0)
    , m_readIndex(0)
    , m_verboseLogging(true)
    , m_requestedVolumePercent(100)
    , m_currentVolumeScale(0.85f)
#if defined(FM_TUNER_HAS_PORTAUDIO)
    , m_paStream(nullptr)
    , m_portAudioInitialized(false)
    , m_outputThreadRunning(false)
    , m_outputReadIndex(0)
#endif
#if defined(__APPLE__) && defined(FM_TUNER_HAS_COREAUDIO)
    , m_audioUnit(nullptr)
    , m_outputReadIndex(0)
#endif
#if defined(_WIN32) && defined(FM_TUNER_HAS_WINMM)
    , m_waveOut(nullptr)
    , m_winmmThreadRunning(false)
    , m_outputReadIndex(0)
#endif
#if defined(__linux__) && defined(FM_TUNER_HAS_ALSA)
    , m_alsaPcm(nullptr)
    , m_alsaThreadRunning(false)
    , m_alsaReadIndex(0)
#endif
{
    m_circularBuffer.resize(65536 * 2);
}

AudioOutput::~AudioOutput() {
    shutdown();
}

bool AudioOutput::init(bool enableSpeaker, const std::string& wavFile, const std::string& deviceSelector, bool verboseLogging) {
    m_enableSpeaker = enableSpeaker;
    m_wavFile = wavFile;
    m_verboseLogging = verboseLogging;

    if (!wavFile.empty()) {
        if (!initWAV(wavFile)) {
            std::cerr << "Failed to initialize WAV file" << std::endl;
            return false;
        }
    }

#if defined(FM_TUNER_HAS_PORTAUDIO)
    if (enableSpeaker) {
        const std::string normalizedSelector = normalizeSelector(deviceSelector);
        if (verboseLogging) {
            std::cout << "[AUDIO] device selector raw='" << deviceSelector
                      << "' normalized='" << normalizedSelector << "'\n";
        }

        PaError err = Pa_Initialize();
        if (err != paNoError) {
            std::cerr << "PortAudio init failed: " << Pa_GetErrorText(err) << std::endl;
            return false;
        } else {
            m_portAudioInitialized = true;
            PaStreamParameters outputParams;
            outputParams.device = selectOutputDevice(normalizedSelector);
            if (outputParams.device == paNoDevice) {
                std::cerr << "PortAudio device not found for selector: " << normalizedSelector << std::endl;
                printOutputDeviceList();
                Pa_Terminate();
                m_portAudioInitialized = false;
                return false;
            }
            outputParams.channelCount = CHANNELS;
            outputParams.sampleFormat = paFloat32;
            const PaDeviceInfo* deviceInfo = Pa_GetDeviceInfo(outputParams.device);
            if (!deviceInfo) {
                std::cerr << "PortAudio failed to get device info for selected output device" << std::endl;
                Pa_Terminate();
                m_portAudioInitialized = false;
                return false;
            }
            outputParams.suggestedLatency = std::max(deviceInfo->defaultLowOutputLatency,
                                                     deviceInfo->defaultHighOutputLatency);
            outputParams.hostApiSpecificStreamInfo = nullptr;
            if (verboseLogging) {
                if (normalizedSelector.empty()) {
                    std::cerr << "Using default output device [" << outputParams.device << "]: " << deviceInfo->name << std::endl;
                } else {
                    std::cerr << "Using selected output device [" << outputParams.device << "]: " << deviceInfo->name << std::endl;
                }
            }

            err = Pa_OpenStream(&m_paStream, nullptr, &outputParams, SAMPLE_RATE, FRAMES_PER_BUFFER, paClipOff, nullptr, nullptr);
            if (err != paNoError) {
                std::cerr << "PortAudio open failed: " << Pa_GetErrorText(err) << std::endl;
                Pa_Terminate();
                m_portAudioInitialized = false;
                return false;
            } else {
                err = Pa_StartStream(m_paStream);
                if (err != paNoError) {
                    std::cerr << "PortAudio start failed: " << Pa_GetErrorText(err) << std::endl;
                    Pa_CloseStream(m_paStream);
                    m_paStream = nullptr;
                    Pa_Terminate();
                    m_portAudioInitialized = false;
                    return false;
                } else if (verboseLogging) {
                    std::cout << "[AUDIO] PortAudio started successfully" << std::endl;
                }

                float primeBuffer[FRAMES_PER_BUFFER * CHANNELS] = {0.0f};
                for (int i = 0; i < 2; i++) {
                    const PaError primeErr = Pa_WriteStream(m_paStream, primeBuffer, FRAMES_PER_BUFFER);
                    if (primeErr != paNoError && primeErr != paOutputUnderflowed) {
                        std::cerr << "PortAudio prime failed: " << Pa_GetErrorText(primeErr) << std::endl;
                        Pa_StopStream(m_paStream);
                        Pa_CloseStream(m_paStream);
                        m_paStream = nullptr;
                        Pa_Terminate();
                        m_portAudioInitialized = false;
                        return false;
                    }
                }

                m_outputThreadRunning = true;
                m_outputThread = std::thread(&AudioOutput::runOutputThread, this);
            }
        }
    }
#endif

#if defined(__APPLE__) && defined(FM_TUNER_HAS_COREAUDIO)
    if (enableSpeaker) {
        const std::string normalizedSelector = normalizeCoreAudioSelector(deviceSelector);
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
        if (AudioComponentInstanceNew(component, &m_audioUnit) != noErr || !m_audioUnit) {
            std::cerr << "[AUDIO] CoreAudio instance creation failed\n";
            m_audioUnit = nullptr;
            return false;
        }

        if (!normalizedSelector.empty()) {
            AudioDeviceID selected = selectOutputDeviceCoreAudio(normalizedSelector);
            if (selected == kAudioObjectUnknown) {
                std::cerr << "[AUDIO] CoreAudio device not found for selector: " << normalizedSelector << "\n";
                listCoreAudioDevices();
                AudioComponentInstanceDispose(m_audioUnit);
                m_audioUnit = nullptr;
                return false;
            }
            const OSStatus selErr = AudioUnitSetProperty(m_audioUnit,
                                                         kAudioOutputUnitProperty_CurrentDevice,
                                                         kAudioUnitScope_Global,
                                                         0,
                                                         &selected,
                                                         sizeof(selected));
            if (selErr != noErr) {
                std::cerr << "[AUDIO] CoreAudio failed to select output device (status=" << selErr << ")\n";
                AudioComponentInstanceDispose(m_audioUnit);
                m_audioUnit = nullptr;
                return false;
            }
        }

        AURenderCallbackStruct callback{};
        callback.inputProc = &AudioOutput::coreAudioRenderCallback;
        callback.inputProcRefCon = this;
        if (AudioUnitSetProperty(m_audioUnit,
                                 kAudioUnitProperty_SetRenderCallback,
                                 kAudioUnitScope_Input,
                                 0,
                                 &callback,
                                 sizeof(callback)) != noErr) {
            std::cerr << "[AUDIO] CoreAudio set render callback failed\n";
            AudioComponentInstanceDispose(m_audioUnit);
            m_audioUnit = nullptr;
            return false;
        }

        AudioStreamBasicDescription asbd{};
        asbd.mSampleRate = static_cast<Float64>(SAMPLE_RATE);
        asbd.mFormatID = kAudioFormatLinearPCM;
        asbd.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | kAudioFormatFlagIsNonInterleaved;
        asbd.mFramesPerPacket = 1;
        asbd.mChannelsPerFrame = CHANNELS;
        asbd.mBitsPerChannel = 32;
        asbd.mBytesPerFrame = sizeof(float);
        asbd.mBytesPerPacket = sizeof(float);
        if (AudioUnitSetProperty(m_audioUnit,
                                 kAudioUnitProperty_StreamFormat,
                                 kAudioUnitScope_Input,
                                 0,
                                 &asbd,
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
            std::cerr << "WinMM device not found for selector: " << normalizedSelector << "\n";
            listWinMMDevices();
            return false;
        }

        WAVEFORMATEX fmt{};
        fmt.wFormatTag = WAVE_FORMAT_PCM;
        fmt.nChannels = static_cast<WORD>(CHANNELS);
        fmt.nSamplesPerSec = SAMPLE_RATE;
        fmt.wBitsPerSample = 16;
        fmt.nBlockAlign = static_cast<WORD>((fmt.nChannels * fmt.wBitsPerSample) / 8);
        fmt.nAvgBytesPerSec = fmt.nBlockAlign * fmt.nSamplesPerSec;
        fmt.cbSize = 0;

        const UINT openId = normalizedSelector.empty() ? WAVE_MAPPER : devId;
        const MMRESULT wr = waveOutOpen(&m_waveOut, openId, &fmt, 0, 0, CALLBACK_NULL);
        if (wr != MMSYSERR_NOERROR || !m_waveOut) {
            std::cerr << "WinMM open failed: " << wr << "\n";
            m_waveOut = nullptr;
            return false;
        }

        m_outputReadIndex = 0;
        m_winmmThreadRunning = true;
        m_winmmThread = std::thread(&AudioOutput::runWinMMOutputThread, this);
        if (verboseLogging) {
            std::cout << "[AUDIO] WinMM started successfully\n";
        }
    }
#endif

#if defined(__linux__) && defined(FM_TUNER_HAS_ALSA)
    if (enableSpeaker) {
        std::string alsaDevice = deviceSelector.empty() ? "default" : deviceSelector;
        if (verboseLogging) {
            std::cout << "[AUDIO] device selector: " << alsaDevice << "\n";
        }
        if (!initAlsa(alsaDevice)) {
            std::cerr << "Failed to initialize ALSA audio output" << std::endl;
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

#if defined(FM_TUNER_HAS_PORTAUDIO)
    m_outputThreadRunning = false;
    m_outputCv.notify_all();
    if (m_outputThread.joinable()) {
        m_outputThread.join();
    }
    if (m_paStream) {
        Pa_StopStream(m_paStream);
        Pa_CloseStream(m_paStream);
        m_paStream = nullptr;
    }
    if (m_portAudioInitialized) {
        Pa_Terminate();
        m_portAudioInitialized = false;
    }
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
    m_outputCv.notify_all();
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
    m_requestedVolumePercent.store(std::clamp(volumePercent, 0, 100), std::memory_order_relaxed);
}

bool AudioOutput::initWAV(const std::string& filename) {
    m_wavHandle = fopen(filename.c_str(), "wb");
    if (!m_wavHandle) {
        return false;
    }

    m_wavDataSize = 0;
    writeWAVHeader();
    return true;
}

void AudioOutput::writeWAVHeader() {
    if (!m_wavHandle) return;

    uint32_t sampleRate = SAMPLE_RATE;
    uint16_t numChannels = CHANNELS;
    uint16_t bitsPerSample = BITS_PER_SAMPLE;
    uint32_t byteRate = sampleRate * numChannels * bitsPerSample / 8;
    uint16_t blockAlign = numChannels * bitsPerSample / 8;
    uint32_t dataSize = m_wavDataSize;

    fseek(m_wavHandle, 0, SEEK_SET);

    fwrite("RIFF", 1, 4, m_wavHandle);
    uint32_t fileSize = 36 + dataSize;
    fwrite(&fileSize, 4, 1, m_wavHandle);
    fwrite("WAVE", 1, 4, m_wavHandle);

    fwrite("fmt ", 1, 4, m_wavHandle);
    uint32_t fmtSize = 16;
    fwrite(&fmtSize, 4, 1, m_wavHandle);
    uint16_t audioFormat = 1;
    fwrite(&audioFormat, 2, 1, m_wavHandle);
    fwrite(&numChannels, 2, 1, m_wavHandle);
    fwrite(&sampleRate, 4, 1, m_wavHandle);
    fwrite(&byteRate, 4, 1, m_wavHandle);
    fwrite(&blockAlign, 2, 1, m_wavHandle);
    fwrite(&bitsPerSample, 2, 1, m_wavHandle);

    fwrite("data", 1, 4, m_wavHandle);
    fwrite(&dataSize, 4, 1, m_wavHandle);
}

bool AudioOutput::writeWAVData(const float* left, const float* right, size_t numSamples) {
    if (!m_wavHandle) return false;

    std::vector<int16_t> buffer(numSamples * CHANNELS);

    for (size_t i = 0; i < numSamples; i++) {
        float l = std::max(-1.0f, std::min(1.0f, left[i]));
        float r = std::max(-1.0f, std::min(1.0f, right[i]));
        buffer[i * 2] = static_cast<int16_t>(l * 32767.0f);
        buffer[i * 2 + 1] = static_cast<int16_t>(r * 32767.0f);
    }

    size_t written = fwrite(buffer.data(), sizeof(int16_t), numSamples * CHANNELS, m_wavHandle);
    m_wavDataSize += written * sizeof(int16_t);

    return written == numSamples * CHANNELS;
}

void AudioOutput::closeWAV() {
    if (m_wavHandle) {
        writeWAVHeader();
        fclose(m_wavHandle);
        m_wavHandle = nullptr;
    }
}

void AudioOutput::clearRealtimeQueue() {
#if defined(FM_TUNER_HAS_PORTAUDIO)
    if (m_paStream) {
        std::lock_guard<std::mutex> lock(m_outputMutex);
        m_outputQueue.clear();
        m_outputReadIndex = 0;
    }
#endif
#if defined(__APPLE__) && defined(FM_TUNER_HAS_COREAUDIO)
    if (m_audioUnit) {
        std::lock_guard<std::mutex> lock(m_outputMutex);
        m_outputQueue.clear();
        m_outputReadIndex = 0;
    }
#endif
#if defined(_WIN32) && defined(FM_TUNER_HAS_WINMM)
    if (m_waveOut) {
        std::lock_guard<std::mutex> lock(m_outputMutex);
        m_outputQueue.clear();
        m_outputReadIndex = 0;
    }
#endif
}

bool AudioOutput::write(const float* left, const float* right, size_t numSamples) {
    if (!m_running) return false;

    std::vector<float> scaledLeft;
    std::vector<float> scaledRight;
    const float* writeLeft = left;
    const float* writeRight = right;

    if (left && right && numSamples > 0) {
        scaledLeft.resize(numSamples);
        scaledRight.resize(numSamples);
        const float targetVolumeScale =
            (static_cast<float>(m_requestedVolumePercent.load(std::memory_order_relaxed)) / 100.0f) * 0.85f;
        const float rampSamples = static_cast<float>(SAMPLE_RATE) * 0.01f;
        const float step = (targetVolumeScale - m_currentVolumeScale) / std::max(1.0f, rampSamples);

        for (size_t i = 0; i < numSamples; i++) {
            if (std::abs(targetVolumeScale - m_currentVolumeScale) > 1e-6f) {
                m_currentVolumeScale += step;
                if ((step > 0.0f && m_currentVolumeScale > targetVolumeScale) ||
                    (step < 0.0f && m_currentVolumeScale < targetVolumeScale)) {
                    m_currentVolumeScale = targetVolumeScale;
                }
            }
            scaledLeft[i] = left[i] * m_currentVolumeScale;
            scaledRight[i] = right[i] * m_currentVolumeScale;
        }
        writeLeft = scaledLeft.data();
        writeRight = scaledRight.data();
    }

    if (m_wavHandle) {
        writeWAVData(writeLeft, writeRight, numSamples);
    }

#if defined(__linux__) && defined(FM_TUNER_HAS_ALSA)
    if (m_enableSpeaker && m_alsaPcm) {
        std::lock_guard<std::mutex> lock(m_alsaMutex);
        constexpr size_t kMaxQueuedSamples = static_cast<size_t>(SAMPLE_RATE) * CHANNELS * 2;
        const size_t queuedSamples = (m_alsaBuffer.size() > m_alsaReadIndex)
            ? (m_alsaBuffer.size() - m_alsaReadIndex)
            : 0;
        const size_t incomingSamples = numSamples * CHANNELS;
        if (queuedSamples + incomingSamples > kMaxQueuedSamples) {
            const size_t drop = (queuedSamples + incomingSamples) - kMaxQueuedSamples;
            m_alsaReadIndex += std::min(drop, queuedSamples);
            if (m_verboseLogging) {
                static std::atomic<uint32_t> dropCount{0};
                const uint32_t count = ++dropCount;
                if (count <= 5 || (count % 50) == 0) {
                    std::cerr << "[AUDIO] ALSA queue overflow (" << count << ")\n";
                }
            }
        }

        const size_t base = m_alsaBuffer.size();
        m_alsaBuffer.resize(base + incomingSamples);
        for (size_t i = 0; i < numSamples; i++) {
            m_alsaBuffer[base + i * 2] = writeLeft[i];
            m_alsaBuffer[base + i * 2 + 1] = writeRight[i];
        }
        m_alsaCv.notify_one();
    }
#endif

#if defined(FM_TUNER_HAS_PORTAUDIO)
    if (m_enableSpeaker && m_paStream) {
        std::lock_guard<std::mutex> lock(m_outputMutex);
        constexpr size_t kMaxQueuedSamples = static_cast<size_t>(SAMPLE_RATE) * CHANNELS;
        const size_t queuedSamples = (m_outputQueue.size() > m_outputReadIndex)
            ? (m_outputQueue.size() - m_outputReadIndex)
            : 0;
        const size_t incomingSamples = numSamples * CHANNELS;
        if (queuedSamples + incomingSamples > kMaxQueuedSamples) {
            const size_t drop = (queuedSamples + incomingSamples) - kMaxQueuedSamples;
            const size_t samplesToRamp = std::min(drop, static_cast<size_t>(FRAMES_PER_BUFFER * CHANNELS * 4));
            if (samplesToRamp > 0) {
                const size_t startIdx = m_outputQueue.size() - samplesToRamp;
                const size_t channels = CHANNELS;
                for (size_t i = 0; i < samplesToRamp; i++) {
                    const float ramp = static_cast<float>(samplesToRamp - i) / static_cast<float>(samplesToRamp);
                    m_outputQueue[startIdx + i] *= ramp;
                }
            }
            m_outputReadIndex += std::min(drop, queuedSamples);
            if (m_verboseLogging) {
                static std::atomic<uint32_t> dropCount{0};
                const uint32_t count = ++dropCount;
                if (count <= 5 || (count % 50) == 0) {
                    std::cerr << "[AUDIO] queue overflow (" << count
                              << ") - ramping out oldest samples to prevent clicks\n";
                }
            }
        }

        const size_t base = m_outputQueue.size();
        m_outputQueue.resize(base + incomingSamples);
        for (size_t i = 0; i < numSamples; i++) {
            m_outputQueue[base + i * 2] = writeLeft[i];
            m_outputQueue[base + i * 2 + 1] = writeRight[i];
        }
        m_outputCv.notify_one();
    }
#endif

#if defined(__APPLE__) && defined(FM_TUNER_HAS_COREAUDIO)
    if (m_enableSpeaker && m_audioUnit) {
        std::lock_guard<std::mutex> lock(m_outputMutex);
        constexpr size_t kMaxQueuedSamples = static_cast<size_t>(SAMPLE_RATE) * CHANNELS;
        const size_t queuedSamples = (m_outputQueue.size() > m_outputReadIndex)
            ? (m_outputQueue.size() - m_outputReadIndex)
            : 0;
        const size_t incomingSamples = numSamples * CHANNELS;
        if (queuedSamples + incomingSamples > kMaxQueuedSamples) {
            const size_t drop = (queuedSamples + incomingSamples) - kMaxQueuedSamples;
            m_outputReadIndex += std::min(drop, queuedSamples);
            if (m_verboseLogging) {
                static std::atomic<uint32_t> dropCount{0};
                const uint32_t count = ++dropCount;
                if (count <= 5 || (count % 50) == 0) {
                    std::cerr << "[AUDIO] CoreAudio queue overflow (" << count << ")\n";
                }
            }
        }
        const size_t base = m_outputQueue.size();
        m_outputQueue.resize(base + incomingSamples);
        for (size_t i = 0; i < numSamples; i++) {
            m_outputQueue[base + i * 2] = writeLeft[i];
            m_outputQueue[base + i * 2 + 1] = writeRight[i];
        }
    }
#endif
#if defined(_WIN32) && defined(FM_TUNER_HAS_WINMM)
    if (m_enableSpeaker && m_waveOut) {
        std::lock_guard<std::mutex> lock(m_outputMutex);
        constexpr size_t kMaxQueuedSamples = static_cast<size_t>(SAMPLE_RATE) * CHANNELS;
        const size_t queuedSamples = (m_outputQueue.size() > m_outputReadIndex)
            ? (m_outputQueue.size() - m_outputReadIndex)
            : 0;
        const size_t incomingSamples = numSamples * CHANNELS;
        if (queuedSamples + incomingSamples > kMaxQueuedSamples) {
            const size_t drop = (queuedSamples + incomingSamples) - kMaxQueuedSamples;
            m_outputReadIndex += std::min(drop, queuedSamples);
            if (m_verboseLogging) {
                static std::atomic<uint32_t> dropCount{0};
                const uint32_t count = ++dropCount;
                if (count <= 5 || (count % 50) == 0) {
                    std::cerr << "[AUDIO] WinMM queue overflow (" << count << ")\n";
                }
            }
        }
        const size_t base = m_outputQueue.size();
        m_outputQueue.resize(base + incomingSamples);
        for (size_t i = 0; i < numSamples; i++) {
            m_outputQueue[base + i * 2] = writeLeft[i];
            m_outputQueue[base + i * 2 + 1] = writeRight[i];
        }
        m_outputCv.notify_one();
    }
#endif

    return true;
}
