// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The close policy, as a truth table. Both halves are pure, so every row is an
// assertion rather than a scenario: the shell performs, this decides.

#include "core/reducer/BackgroundMode.h"

#include <catch2/catch_test_macros.hpp>

using dish::reducer::CloseAction;
using dish::reducer::decideCloseAction;
using dish::reducer::shouldAnnounceBackground;

TEST_CASE("decideCloseAction: enabled with a tray host hides to the background", "[background]") {
    REQUIRE(decideCloseAction(true, true) == CloseAction::HideToBackground);
}

TEST_CASE("decideCloseAction: enabled but no tray still quits", "[background]") {
    // The safety property. Hiding with no tray host leaves a running Dish behind
    // no window and no menu, so the preference alone is never enough: on a
    // desktop without a StatusNotifier host, closing has to mean quitting.
    REQUIRE(decideCloseAction(true, false) == CloseAction::Quit);
}

TEST_CASE("decideCloseAction: disabled quits even where a tray host exists", "[background]") {
    REQUIRE(decideCloseAction(false, true) == CloseAction::Quit);
}

TEST_CASE("decideCloseAction: disabled with no tray quits", "[background]") {
    REQUIRE(decideCloseAction(false, false) == CloseAction::Quit);
}

TEST_CASE("shouldAnnounceBackground: the first hide is announced", "[background]") {
    // A window that vanishes without a word reads as a crash.
    REQUIRE(shouldAnnounceBackground(CloseAction::HideToBackground, false));
}

TEST_CASE("shouldAnnounceBackground: a later hide is silent", "[background]") {
    REQUIRE_FALSE(shouldAnnounceBackground(CloseAction::HideToBackground, true));
}

TEST_CASE("shouldAnnounceBackground: a quit is never announced", "[background]") {
    // Nothing keeps running, so there is nothing to tell the user about.
    REQUIRE_FALSE(shouldAnnounceBackground(CloseAction::Quit, false));
}

TEST_CASE("shouldAnnounceBackground: a quit stays silent once announced", "[background]") {
    REQUIRE_FALSE(shouldAnnounceBackground(CloseAction::Quit, true));
}
