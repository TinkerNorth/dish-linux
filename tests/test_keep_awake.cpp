// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The keep-awake policy, as a truth table. Every branch is pure, so the whole
// decision space is assertions rather than scenarios.

#include "core/reducer/KeepAwake.h"

#include <catch2/catch_test_macros.hpp>

using dish::reducer::clampKeepAwakeTimeoutMinutes;
using dish::reducer::controllerActiveAt;
using dish::reducer::deriveKeepAwakeReach;
using dish::reducer::KeepAwakeMode;
using dish::reducer::keepAwakeModeFromKey;
using dish::reducer::keepAwakeModeKey;
using dish::reducer::KeepAwakePreferences;
using dish::reducer::KeepAwakeReach;
using dish::reducer::kKeepAwakeDefaultTimeoutMinutes;
using dish::reducer::kKeepAwakeMaxTimeoutMinutes;
using dish::reducer::kKeepAwakeMinTimeoutMinutes;

namespace {

KeepAwakePreferences prefs(KeepAwakeMode mode, bool display = false, int minutes = 5) {
    return KeepAwakePreferences{mode, minutes, display};
}

constexpr std::int64_t kMinute = 60'000;

} // namespace

TEST_CASE("KeepAwakePreferences: the defaults hold the machine, not the panel", "[keepawake]") {
    const KeepAwakePreferences p;
    REQUIRE(p.mode == KeepAwakeMode::WhileControllerActive);
    REQUIRE(p.idleTimeoutMinutes == kKeepAwakeDefaultTimeoutMinutes);
    REQUIRE_FALSE(p.keepDisplayAwake);
}

TEST_CASE("KeepAwakePreferences: equality covers every field", "[keepawake]") {
    REQUIRE(prefs(KeepAwakeMode::WhileConnected) == prefs(KeepAwakeMode::WhileConnected));
    REQUIRE(prefs(KeepAwakeMode::WhileConnected) != prefs(KeepAwakeMode::Off));
    REQUIRE(prefs(KeepAwakeMode::WhileConnected) != prefs(KeepAwakeMode::WhileConnected, true));
    REQUIRE(prefs(KeepAwakeMode::WhileConnected) != prefs(KeepAwakeMode::WhileConnected, false, 6));
}

TEST_CASE("clampKeepAwakeTimeoutMinutes: pins the ends of the range", "[keepawake]") {
    REQUIRE(clampKeepAwakeTimeoutMinutes(0) == kKeepAwakeMinTimeoutMinutes);
    REQUIRE(clampKeepAwakeTimeoutMinutes(-90) == kKeepAwakeMinTimeoutMinutes);
    REQUIRE(clampKeepAwakeTimeoutMinutes(kKeepAwakeMinTimeoutMinutes) ==
            kKeepAwakeMinTimeoutMinutes);
    REQUIRE(clampKeepAwakeTimeoutMinutes(5) == 5);
    REQUIRE(clampKeepAwakeTimeoutMinutes(kKeepAwakeMaxTimeoutMinutes) ==
            kKeepAwakeMaxTimeoutMinutes);
    REQUIRE(clampKeepAwakeTimeoutMinutes(kKeepAwakeMaxTimeoutMinutes + 1) ==
            kKeepAwakeMaxTimeoutMinutes);
}

TEST_CASE("keepAwakeModeKey: round-trips every mode", "[keepawake]") {
    for (const auto mode : {KeepAwakeMode::Off, KeepAwakeMode::WhileControllerActive,
                            KeepAwakeMode::WhileConnected}) {
        REQUIRE(keepAwakeModeFromKey(keepAwakeModeKey(mode)) == mode);
    }
}

TEST_CASE("keepAwakeModeFromKey: an unknown key cannot pin the machine awake", "[keepawake]") {
    // The lenient fallback is the timed mode, never the unbounded one.
    REQUIRE(keepAwakeModeFromKey("") == KeepAwakeMode::WhileControllerActive);
    REQUIRE(keepAwakeModeFromKey("nonsense") == KeepAwakeMode::WhileControllerActive);
    REQUIRE(keepAwakeModeFromKey("OFF") == KeepAwakeMode::WhileControllerActive);
}

TEST_CASE("controllerActiveAt: zero is a stamp, not a never-seen sentinel", "[keepawake]") {
    // "Never seen input" is the caller's flag, not a magic timestamp: a
    // monotonic clock can legitimately read 0.
    REQUIRE(controllerActiveAt(0, 0, 5 * kMinute));
    REQUIRE(controllerActiveAt(0, kMinute, 5 * kMinute));
    REQUIRE_FALSE(controllerActiveAt(0, 10 * kMinute, 5 * kMinute));
}

TEST_CASE("controllerActiveAt: inside the window is active, the boundary is not", "[keepawake]") {
    REQUIRE(controllerActiveAt(kMinute, kMinute + 1, 5 * kMinute));
    REQUIRE(controllerActiveAt(kMinute, 5 * kMinute, 5 * kMinute));
    REQUIRE_FALSE(controllerActiveAt(kMinute, 6 * kMinute, 5 * kMinute));
    REQUIRE_FALSE(controllerActiveAt(kMinute, 600 * kMinute, 5 * kMinute));
}

TEST_CASE("controllerActiveAt: a non-positive timeout never expires", "[keepawake]") {
    REQUIRE(controllerActiveAt(kMinute, 10'000 * kMinute, 0));
    REQUIRE(controllerActiveAt(kMinute, 10'000 * kMinute, -1));
}

TEST_CASE("controllerActiveAt: a backwards clock reads as activity", "[keepawake]") {
    // Better a hold that outlives its window than a machine that suspends
    // mid-game because the clock stepped.
    REQUIRE(controllerActiveAt(10 * kMinute, 2 * kMinute, 5 * kMinute));
    REQUIRE(controllerActiveAt(10 * kMinute, 10 * kMinute, 5 * kMinute));
}

TEST_CASE("deriveKeepAwakeReach: Off never holds, whatever else is true", "[keepawake]") {
    REQUIRE(deriveKeepAwakeReach(prefs(KeepAwakeMode::Off), 2, true) == KeepAwakeReach::None);
    REQUIRE(deriveKeepAwakeReach(prefs(KeepAwakeMode::Off, true), 2, true) == KeepAwakeReach::None);
}

TEST_CASE("deriveKeepAwakeReach: nothing streaming never holds", "[keepawake]") {
    REQUIRE(deriveKeepAwakeReach(prefs(KeepAwakeMode::WhileConnected), 0, true) ==
            KeepAwakeReach::None);
    REQUIRE(deriveKeepAwakeReach(prefs(KeepAwakeMode::WhileControllerActive), 0, true) ==
            KeepAwakeReach::None);
    REQUIRE(deriveKeepAwakeReach(prefs(KeepAwakeMode::WhileConnected), -1, true) ==
            KeepAwakeReach::None);
}

TEST_CASE("deriveKeepAwakeReach: WhileConnected ignores controller activity", "[keepawake]") {
    REQUIRE(deriveKeepAwakeReach(prefs(KeepAwakeMode::WhileConnected), 1, false) ==
            KeepAwakeReach::System);
    REQUIRE(deriveKeepAwakeReach(prefs(KeepAwakeMode::WhileConnected), 1, true) ==
            KeepAwakeReach::System);
}

TEST_CASE("deriveKeepAwakeReach: WhileControllerActive follows activity", "[keepawake]") {
    REQUIRE(deriveKeepAwakeReach(prefs(KeepAwakeMode::WhileControllerActive), 1, true) ==
            KeepAwakeReach::System);
    REQUIRE(deriveKeepAwakeReach(prefs(KeepAwakeMode::WhileControllerActive), 1, false) ==
            KeepAwakeReach::None);
}

TEST_CASE("deriveKeepAwakeReach: the display opt-in widens a hold it never creates",
          "[keepawake]") {
    REQUIRE(deriveKeepAwakeReach(prefs(KeepAwakeMode::WhileConnected, true), 1, false) ==
            KeepAwakeReach::SystemAndDisplay);
    REQUIRE(deriveKeepAwakeReach(prefs(KeepAwakeMode::WhileControllerActive, true), 1, false) ==
            KeepAwakeReach::None);
    REQUIRE(deriveKeepAwakeReach(prefs(KeepAwakeMode::WhileConnected, true), 0, true) ==
            KeepAwakeReach::None);
}
