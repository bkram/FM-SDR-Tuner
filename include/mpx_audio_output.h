#ifndef MPX_AUDIO_OUTPUT_H
#define MPX_AUDIO_OUTPUT_H

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "dsp/liquid_primitives.h"

#if defined(__APPLE__) && defined(FM_TUNER_HAS_COREAUDIO)
#include <AudioUnit/AudioUnit.h>
#elif defined(_WIN32) && defined(FM_TUNER_HAS_WINMM)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>
#elif defined(__linux__) && defined(FM_TUNER_HAS_ALSA)
#include <alsa/asoundlib.h>
#endif

// Live PCM sink for the multiplex (MPX) baseband signal coming out of the FM
// discriminator. Mono, runs at a user-selectable sample rate (typically
// 192 kHz so the 19/38/57 kHz subcarriers are preserved). Resamples from the
// post-decimation IQ rate on the producer side via liquid_dsp.
//
// Primarily intended to feed a virtual loopback device (BlackHole / snd-aloop
// / VB-CABLE) so downstream tools — RDS decoders, spectrum analyzers, FM
// exciters that accept raw MPX — can capture the live MPX without going
// through file I/O.
class MpxAudioOutput {
public:
  MpxAudioOutput();
  ~MpxAudioOutput();

  bool init(const std::string &deviceSelector, uint32_t sourceSampleRate,
            uint32_t targetSampleRate, bool verboseLogging);
  void shutdown();
  bool isOpen() const { return m_running.load(); }

  // Linear gain applied at enqueue. Used for MPX headroom: the demod
  // calibrates 75 kHz deviation to 1.0 full scale, so real broadcasts
  // (which deviate to and beyond 75 kHz) would otherwise clip in the
  // device-format conversion.
  void setGain(float gain) { m_gain = gain; }

  // Push MPX samples at the source rate. Internally resampled (if needed) to
  // the target rate before being queued to the device.
  bool enqueueMpx(const float *samples, size_t sampleCount);

private:
  void clearQueueLocked();
  void runAlsaThread();
  void runWinMMThread();

#if defined(__APPLE__) && defined(FM_TUNER_HAS_COREAUDIO)
  static OSStatus coreAudioRenderCallback(
      void *inRefCon, AudioUnitRenderActionFlags *ioActionFlags,
      const AudioTimeStamp *inTimeStamp, UInt32 inBusNumber,
      UInt32 inNumberFrames, AudioBufferList *ioData);
#endif

  uint32_t m_sourceRate;
  uint32_t m_targetRate;
  // Output channel count. Always 1 on the producer side (MPX is mono), but
  // some Core Audio devices reject a mono StreamFormat property — the render
  // callback then duplicates the same MPX sample to L and R via this field.
  unsigned int m_outChannels = 1;
  bool m_verboseLogging;
  // Linear pre-conversion gain; see setGain().
  float m_gain = 1.0f;

  std::atomic<bool> m_running;

  // Producer-side resampler (source → target rate) and scratch buffer.
  bool m_resampleEnabled;
  fm_tuner::dsp::liquid::Resampler m_resampler;
  std::array<float, fm_tuner::dsp::liquid::Resampler::kMaxOutput>
      m_resampleTmp{};

  // Ring buffer of float mono samples at target rate.
  std::mutex m_mutex;
  std::condition_variable m_cv;
  std::vector<float> m_ring;
  size_t m_readPos;
  size_t m_writePos;
  size_t m_size;

#if defined(__APPLE__) && defined(FM_TUNER_HAS_COREAUDIO)
  AudioUnit m_audioUnit;
#endif
#if defined(__linux__) && defined(FM_TUNER_HAS_ALSA)
  snd_pcm_t *m_alsaPcm;
  std::thread m_alsaThread;
  std::atomic<bool> m_alsaThreadRunning;
#endif
#if defined(_WIN32) && defined(FM_TUNER_HAS_WINMM)
  HWAVEOUT m_waveOut;
  std::thread m_winmmThread;
  std::atomic<bool> m_winmmThreadRunning;
#endif
};

#endif
