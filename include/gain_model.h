#ifndef GAIN_MODEL_H
#define GAIN_MODEL_H

// The tuner's gain model: one place that decides what gain the front end runs
// at and whether the front end's own AGC is in charge.
//
// The tuner owns this state so a setting survives a retune, a periodic
// re-assert and an auto-gain step. A client control (the fm-dx-webserver
// plugin over REST, an XDR client over the XDR protocol) must go through the
// same state; writing the hardware registers directly from a client handler
// is undone by the next re-assert, and the software auto-gain ladder then
// fights the front end's own AGC over the same register.

namespace fm_tuner {

// Packed custom-gain flags: the tens digit is the RF flag (4 dB of extra RF
// gain), the ones digit is the IF flag (the front end's own IF AGC).
inline int customGainRfFlag(int customFlags) {
  return ((customFlags / 10) % 10) ? 1 : 0;
}
inline int customGainIfFlag(int customFlags) { return (customFlags % 10) ? 1 : 0; }

// The gain ladder: A0 is the most gain, A3 the least. The direct-RTL table is
// in dB of tuner gain; the TEF table is the strategy's own scale.
inline int ladderGainDb(int mode, bool directRtlSdr) {
  const int m = (mode < 0) ? 0 : ((mode > 3) ? 3 : mode);
  static constexpr int kDirectRtl[4] = {18, 12, 7, 0};
  static constexpr int kTef[4] = {44, 36, 30, 24};
  return directRtlSdr ? kDirectRtl[m] : kTef[m];
}

// The gain the tuner applies, in dB.
//
// manualGainDb is the runtime manual gain (>= 0): it is what a client set
// through the REST gain control, or the value the profile started with. -1
// means "no manual gain" and leaves the decision to the ladder.
inline int appliedGainDb(int ladderMode, int customFlags, int manualGainDb,
                          bool directRtlSdr) {
  if (manualGainDb >= 0) {
    return (manualGainDb > 49) ? 49 : manualGainDb;
  }
  int db = ladderGainDb(ladderMode, directRtlSdr);
  if (customGainRfFlag(customFlags)) {
    db += 4;
  }
  return (db > 49) ? 49 : db;
}

// Does the front end's own IF AGC drive the level?
//
// It may only do so when the gain is not pinned to a manual value: a pinned
// gain and a hardware AGC contradict each other, and the hardware wins, so
// the manual gain the user asked for would silently not apply. With a pinned
// gain the IF AGC stays off and the software ladder stands down as well.
inline bool hardwareIfAgcActive(int customFlags, int manualGainDb) {
  if (manualGainDb >= 0) {
    return false;
  }
  return customGainIfFlag(customFlags) != 0;
}

// Does the software auto-gain ladder get to act? It may only when the gain is
// not pinned: a manual gain (from the profile or from a client) is the
// user's decision and the ladder must not step it out from under them.
inline bool autoGainLadderActive(int manualGainDb) {
  return manualGainDb < 0;
}

} // namespace fm_tuner

#endif
