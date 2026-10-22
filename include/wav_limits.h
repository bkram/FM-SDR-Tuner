#ifndef WAV_LIMITS_H
#define WAV_LIMITS_H

#include <cstddef>
#include <cstdint>

// A RIFF/WAVE container stores the chunk sizes in 32-bit fields, so a file
// whose payload does not fit in 32 bits cannot describe its own length: the
// size fields wrap and the file reads back as truncated (or as garbage).
//
// The payload limit is therefore 4 GiB minus the container overhead (the
// "RIFF" chunk size field also has to cover the 36 bytes that are not
// payload: the chunk headers and the "fmt " chunk).
//
// Both writers in this project — WavWriter (the MPX capture) and
// AudioOutput's WAV path (the audio capture) — stop at the same limit and
// treat reaching it as a fatal error, so the rule lives here once.
constexpr size_t kWavFileOverheadBytes = 36;
constexpr size_t kWavMaxDataBytes =
    static_cast<size_t>(UINT32_MAX) - kWavFileOverheadBytes;

// True when `addBytes` more of payload still fit in a RIFF/WAVE container.
inline bool wavDataFits(size_t currentBytes, size_t addBytes) {
  if (currentBytes > kWavMaxDataBytes) {
    return false;
  }
  return addBytes <= (kWavMaxDataBytes - currentBytes);
}

#endif
