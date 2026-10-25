#ifndef RETUNE_POLICY_H
#define RETUNE_POLICY_H

#include <cstdint>

namespace fm_tuner {

// A tune request that names the frequency the tuner is already on must not
// retune.
//
// A retune is not a no-op: it reprograms the tuner, re-asserts the gain and
// AGC registers, flushes the audio queue, resets the DSP and mutes the output
// for tens of milliseconds while the demodulator settles. The listener hears
// that as the programme restarting -- the station "repeats" itself.
//
// Clients re-send the frequency they are already on routinely (xdr-gtk on
// every click of the tuned entry, the REST panel on every poll that carries a
// frequency), so this is the common case rather than the exception.
//
// `appliedFrequencyHz` is the frequency the tuner is actually on, or 0 when
// that is not known -- an unknown frequency always retunes.
inline bool isRedundantRetune(uint32_t targetFrequencyHz,
                              uint32_t appliedFrequencyHz) {
  return appliedFrequencyHz != 0 && targetFrequencyHz == appliedFrequencyHz;
}

} // namespace fm_tuner

#endif // RETUNE_POLICY_H
