#include "gain_model.h"

#include <catch2/catch_test_macros.hpp>

using namespace fm_tuner;

TEST_CASE("The gain ladder is the strategy's own table", "[gain_model]") {
  SECTION("the direct RTL table is 18/12/7/0 dB") {
    REQUIRE(ladderGainDb(0, true) == 18);
    REQUIRE(ladderGainDb(1, true) == 12);
    REQUIRE(ladderGainDb(2, true) == 7);
    REQUIRE(ladderGainDb(3, true) == 0);
  }
  SECTION("the TEF table is the strategy's scale") {
    REQUIRE(ladderGainDb(0, false) == 44);
    REQUIRE(ladderGainDb(3, false) == 24);
  }
  SECTION("the mode is clamped to the ladder") {
    REQUIRE(ladderGainDb(-1, true) == 18);
    REQUIRE(ladderGainDb(9, true) == 0);
  }
}

TEST_CASE("A manual gain wins over the ladder", "[gain_model]") {
  SECTION("the manual gain is applied as it is") {
    REQUIRE(appliedGainDb(2, 0, 10, true) == 10);
    REQUIRE(appliedGainDb(0, 0, 7, true) == 7);
  }
  SECTION("the manual gain is clamped to the tuner's range") {
    REQUIRE(appliedGainDb(0, 0, 60, true) == 49);
  }
  SECTION("without a manual gain the ladder decides") {
    REQUIRE(appliedGainDb(0, 0, -1, true) == 18);
    REQUIRE(appliedGainDb(2, 0, -1, true) == 7);
  }
  SECTION("the RF flag adds 4 dB on top of the ladder") {
    REQUIRE(appliedGainDb(2, 10, -1, true) == 11);
    REQUIRE(appliedGainDb(2, 1, -1, true) == 7);
  }
}

TEST_CASE("The front end's own AGC stands down for a manual gain",
          "[gain_model]") {
  SECTION("the IF flag turns the front end's AGC on") {
    REQUIRE(hardwareIfAgcActive(1, -1));
    REQUIRE(hardwareIfAgcActive(11, -1));
  }
  SECTION("without the IF flag it stays off") {
    REQUIRE_FALSE(hardwareIfAgcActive(0, -1));
    REQUIRE_FALSE(hardwareIfAgcActive(10, -1));
  }
  SECTION("a pinned manual gain switches it off again") {
    // A manual gain and a hardware AGC contradict each other: the hardware
    // wins and the manual gain would silently not apply.
    REQUIRE_FALSE(hardwareIfAgcActive(1, 10));
  }
}

TEST_CASE("The auto-gain ladder only acts on an unpinned gain",
          "[gain_model]") {
  REQUIRE(autoGainLadderActive(-1));
  REQUIRE_FALSE(autoGainLadderActive(10));
  REQUIRE_FALSE(autoGainLadderActive(0));
}
