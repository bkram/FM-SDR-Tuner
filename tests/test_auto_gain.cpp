#include "catch_compat.h"

#include "auto_gain_policy.h"

using fm_tuner::AutoGainStep;
using fm_tuner::decideAutoGainStep;

namespace {
constexpr double kNoClip = 0.0;
constexpr double kHeavyClip = 0.10;
}

// The regression this guards: the auto-gain ladder used to only climb back when
// the signal was very weak, so on a normal-strength station it was a one-way
// ratchet — transient overloads walked the gain down to the minimum rung over
// hours and it never recovered until a restart.
TEST_CASE("Auto-gain climbs back from a low rung when there is headroom",
          "[auto_gain]") {
  // Stuck at A3 (minimum gain) on a perfectly clean, moderate signal with
  // plenty of headroom below the overload point. It MUST step up.
  REQUIRE(decideAutoGainStep(/*mode=*/3, kNoClip, /*dbfs=*/-25.0,
                             /*downElapsed=*/true, /*upElapsed=*/true) ==
          AutoGainStep::Up);
  // Same at an intermediate rung.
  REQUIRE(decideAutoGainStep(2, kNoClip, -30.0, true, true) ==
          AutoGainStep::Up);
}

TEST_CASE("Auto-gain steps down on overload", "[auto_gain]") {
  REQUIRE(decideAutoGainStep(1, kHeavyClip, -10.0, true, true) ==
          AutoGainStep::Down);
  // dbfs near full scale is also an overload even without a high clip ratio.
  REQUIRE(decideAutoGainStep(1, kNoClip, -2.0, true, true) ==
          AutoGainStep::Down);
}

TEST_CASE("Auto-gain holds inside the dead-band (no pumping)", "[auto_gain]") {
  // Between the -20 dBFS climb threshold and the -5 dBFS overload threshold:
  // neither up nor down, so a settled signal doesn't oscillate.
  REQUIRE(decideAutoGainStep(1, kNoClip, -12.0, true, true) ==
          AutoGainStep::None);
  REQUIRE(decideAutoGainStep(1, kNoClip, -6.0, true, true) ==
          AutoGainStep::None);
}

TEST_CASE("Auto-gain respects the rung limits", "[auto_gain]") {
  // Already at the least-gain rung: cannot step down further.
  REQUIRE(decideAutoGainStep(3, kHeavyClip, -1.0, true, true) ==
          AutoGainStep::None);
  // Already at the most-gain rung: cannot step up further.
  REQUIRE(decideAutoGainStep(0, kNoClip, -40.0, true, true) ==
          AutoGainStep::None);
}

TEST_CASE("Auto-gain waits for the hysteresis timers", "[auto_gain]") {
  // Overload but the down timer hasn't elapsed -> hold.
  REQUIRE(decideAutoGainStep(1, kHeavyClip, -1.0, /*downElapsed=*/false,
                             /*upElapsed=*/true) == AutoGainStep::None);
  // Headroom but the up timer hasn't elapsed -> hold.
  REQUIRE(decideAutoGainStep(2, kNoClip, -30.0, /*downElapsed=*/true,
                             /*upElapsed=*/false) == AutoGainStep::None);
}
