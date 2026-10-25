#include "catch_compat.h"
#include "retune_policy.h"

TEST_CASE("a tune request for the frequency already tuned does not retune",
          "[retune_policy]") {
  // The tuner is on 96.2 MHz and the client asks for 96.2 MHz: nothing to do.
  REQUIRE(fm_tuner::isRedundantRetune(96200000U, 96200000U));

  // A different frequency always retunes.
  REQUIRE_FALSE(fm_tuner::isRedundantRetune(96200000U, 96100000U));

  // An unknown applied frequency (0) always retunes: the tuner might be
  // anywhere, and a missed retune would leave it on the wrong station.
  REQUIRE_FALSE(fm_tuner::isRedundantRetune(96200000U, 0U));
}
