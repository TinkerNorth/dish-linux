// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "core/reducer/RumbleRouting.h"

#include <catch2/catch_test_macros.hpp>

#include <vector>

using dish::reducer::combinedRumblePlan;
using dish::reducer::isRumbleStop;
using dish::reducer::RumbleCommand;
using dish::reducer::rumbleTheUserAllows;

TEST_CASE("combinedRumblePlan separates strong and weak across two actuators", "[rumble][plan]") {
    const auto plan = combinedRumblePlan(2, 200, 100);
    REQUIRE(plan.size() == 2U);
    REQUIRE(plan[0] == std::make_pair(0, 200));
    REQUIRE(plan[1] == std::make_pair(1, 100));
}

TEST_CASE("combinedRumblePlan drops a zero-strong actuator on a dual target", "[rumble][plan]") {
    const auto plan = combinedRumblePlan(2, 0, 100);
    REQUIRE(plan.size() == 1U);
    REQUIRE(plan[0] == std::make_pair(1, 100));
}

TEST_CASE("combinedRumblePlan drops a zero-weak actuator on a dual target", "[rumble][plan]") {
    const auto plan = combinedRumblePlan(2, 200, 0);
    REQUIRE(plan.size() == 1U);
    REQUIRE(plan[0] == std::make_pair(0, 200));
}

TEST_CASE("combinedRumblePlan yields nothing when both amplitudes are zero on a dual target",
          "[rumble][plan]") {
    REQUIRE(combinedRumblePlan(2, 0, 0).empty());
}

TEST_CASE("combinedRumblePlan folds a strong-dominant effect onto a single actuator",
          "[rumble][plan]") {
    const auto plan = combinedRumblePlan(1, 200, 50);
    REQUIRE(plan.size() == 1U);
    REQUIRE(plan[0] == std::make_pair(0, 200));
}

TEST_CASE("combinedRumblePlan folds a weak-dominant effect onto a single actuator",
          "[rumble][plan]") {
    const auto plan = combinedRumblePlan(1, 40, 180);
    REQUIRE(plan.size() == 1U);
    REQUIRE(plan[0] == std::make_pair(0, 180));
}

TEST_CASE("combinedRumblePlan drives the single actuator when only weak is set", "[rumble][plan]") {
    const auto plan = combinedRumblePlan(1, 0, 90);
    REQUIRE(plan.size() == 1U);
    REQUIRE(plan[0] == std::make_pair(0, 90));
}

TEST_CASE("combinedRumblePlan yields nothing for a single actuator with no amplitude",
          "[rumble][plan]") {
    REQUIRE(combinedRumblePlan(1, 0, 0).empty());
}

TEST_CASE("combinedRumblePlan yields nothing when there are no actuators", "[rumble][plan]") {
    REQUIRE(combinedRumblePlan(0, 200, 100).empty());
}

TEST_CASE("isRumbleStop is true when both magnitudes are zero or duration is zero",
          "[rumble][stop]") {
    REQUIRE(isRumbleStop(0, 0, 100));
    REQUIRE(isRumbleStop(500, 500, 0));
}

TEST_CASE("isRumbleStop is false when there is a positive magnitude and duration",
          "[rumble][stop]") {
    REQUIRE_FALSE(isRumbleStop(500, 0, 100));
    REQUIRE_FALSE(isRumbleStop(0, 500, 100));
}

TEST_CASE("rumble the user left on reaches the pad as the host sent it", "[rumble][switch]") {
    const RumbleCommand fromHost{40000, 12000, 500};
    CHECK(rumbleTheUserAllows(fromHost, true) == fromHost);
}

TEST_CASE("rumble the user switched off reaches the pad with both motors stopped",
          "[rumble][switch]") {
    const RumbleCommand heldUntilTheNext{40000, 12000, 0};
    const RumbleCommand delivered = rumbleTheUserAllows(heldUntilTheNext, false);
    CHECK(delivered.strong == 0);
    CHECK(delivered.weak == 0);
}
