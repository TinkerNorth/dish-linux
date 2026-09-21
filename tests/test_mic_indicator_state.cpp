// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The app-wide mic chip folds every bound slot to one of three answers, and
// its one click leaves the machine in one state. Mirrors dish-android's
// MicIndicatorPolicy tests rule for rule.

#include "core/reducer/MicIndicatorState.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using dish::reducer::micIndicatorFor;
using dish::reducer::MicIndicatorState;
using dish::reducer::micToggleAllFor;

TEST_CASE("mic chip: nothing armed hides the surface", "[mic][indicator]") {
    REQUIRE(micIndicatorFor(0, 0) == MicIndicatorState::Hidden);
    // Capturing without armed cannot happen (capturing is a subset), but the
    // fold must not invent a microphone from a bad count either.
    REQUIRE(micIndicatorFor(0, 1) == MicIndicatorState::Hidden);
}

TEST_CASE("mic chip: any delivering slot reads Live", "[mic][indicator]") {
    REQUIRE(micIndicatorFor(1, 1) == MicIndicatorState::Live);
    // A mixed set (one live, one muted) is Live: the microphone is hot.
    REQUIRE(micIndicatorFor(2, 1) == MicIndicatorState::Live);
}

TEST_CASE("mic chip: armed with every slot muted reads Muted", "[mic][indicator]") {
    REQUIRE(micIndicatorFor(1, 0) == MicIndicatorState::Muted);
    REQUIRE(micIndicatorFor(3, 0) == MicIndicatorState::Muted);
}

TEST_CASE("mic chip click: a Live set mutes every armed slot, a Muted set unmutes every one",
          "[mic][toggle]") {
    const std::vector<std::string> armed = {"slot-a", "slot-b"};

    // Mixed live-and-muted still counts as Live, so the click silences ALL of it.
    const auto mute = micToggleAllFor(armed, /*capturingSlots=*/1);
    REQUIRE(mute.has_value());
    CHECK(mute->muted);
    CHECK(mute->slotIds == armed);

    const auto unmute = micToggleAllFor(armed, /*capturingSlots=*/0);
    REQUIRE(unmute.has_value());
    CHECK_FALSE(unmute->muted);
    CHECK(unmute->slotIds == armed);
}

TEST_CASE("mic chip click: nothing armed is a no-op, never a stale write", "[mic][toggle]") {
    REQUIRE_FALSE(micToggleAllFor({}, 0).has_value());
    REQUIRE_FALSE(micToggleAllFor({}, 1).has_value());
}
