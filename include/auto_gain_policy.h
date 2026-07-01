#ifndef AUTO_GAIN_POLICY_H
#define AUTO_GAIN_POLICY_H

// Pure decision logic for the RTL/TEF auto-gain ladder, split out from
// runtime_loop.cpp so it can be unit-tested without the whole runtime.
//
// The gain ladder has four rungs A0..A3 (A0 = most gain, A3 = least). This
// decides whether to step DOWN one rung (less gain, clip-protect) or UP one
// rung (more gain, sensitivity) for this block.
//
// The important property is that recovery (UP) keys off raw-dBFS *headroom*,
// not a strict "signal is weak" test. An earlier version only climbed when the
// signal was very weak, which made the ladder a one-way ratchet: transient
// overloads stepped it down over hours, but a normal-strength station never
// looked "weak", so the gain never recovered and reception slowly degraded to
// the minimum rung until the app was restarted. The DOWN threshold (dbfs > -5)
// and UP threshold (dbfs < -20) leave a 15 dB dead-band, wider than the ~5-8 dB
// gain steps, so the servo settles instead of pumping.

namespace fm_tuner {

enum class AutoGainStep { None, Down, Up };

// currentMode: 0..3 (A0..A3). timers indicate whether the down/up hysteresis
// interval has elapsed. clipRatio and dbfs are the current block's raw IQ clip
// ratio and channel dBFS.
inline AutoGainStep decideAutoGainStep(int currentMode, double clipRatio,
                                       double dbfs, bool downTimerElapsed,
                                       bool upTimerElapsed) {
  const bool overload = (clipRatio > 0.0200) || (dbfs > -5.0);
  const bool hasHeadroom = (clipRatio < 0.0005) && (dbfs < -20.0);

  if (overload && downTimerElapsed && currentMode < 3) {
    return AutoGainStep::Down;
  }
  if (hasHeadroom && upTimerElapsed && currentMode > 0) {
    return AutoGainStep::Up;
  }
  return AutoGainStep::None;
}

} // namespace fm_tuner

#endif
