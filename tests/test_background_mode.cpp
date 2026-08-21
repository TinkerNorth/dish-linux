// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The close policy, as a truth table. Both halves are pure, so every row is an
// assertion rather than a scenario: the shell performs, this decides.

#include "core/reducer/BackgroundMode.h"

#include <catch2/catch_test_macros.hpp>

using dish::reducer::decideWindowCloseAction;
using dish::reducer::shouldAnnounceBackground;
using dish::reducer::WindowCloseAction;

TEST_CASE("decideWindowCloseAction: enabled with a tray host hides to the background",
          "[background]") {
    REQUIRE(decideWindowCloseAction(true, true) == WindowCloseAction::HideToBackground);
}

TEST_CASE("decideWindowCloseAction: enabled but no tray still quits", "[background]") {
    // The safety property. Hiding with no tray host leaves a running Dish behind
    // no window and no menu, so the preference alone is never enough: on a
    // desktop without a StatusNotifier host, closing has to mean quitting.
    REQUIRE(decideWindowCloseAction(true, false) == WindowCloseAction::Quit);
}

TEST_CASE("decideWindowCloseAction: disabled quits even where a tray host exists", "[background]") {
    REQUIRE(decideWindowCloseAction(false, true) == WindowCloseAction::Quit);
}

TEST_CASE("decideWindowCloseAction: disabled with no tray quits", "[background]") {
    REQUIRE(decideWindowCloseAction(false, false) == WindowCloseAction::Quit);
}

TEST_CASE("shouldAnnounceBackground: the first hide is announced", "[background]") {
    // A window that vanishes without a word reads as a crash.
    REQUIRE(shouldAnnounceBackground(WindowCloseAction::HideToBackground, false));
}

TEST_CASE("shouldAnnounceBackground: a later hide is silent", "[background]") {
    REQUIRE_FALSE(shouldAnnounceBackground(WindowCloseAction::HideToBackground, true));
}

TEST_CASE("shouldAnnounceBackground: a quit is never announced", "[background]") {
    // Nothing keeps running, so there is nothing to tell the user about.
    REQUIRE_FALSE(shouldAnnounceBackground(WindowCloseAction::Quit, false));
}

TEST_CASE("shouldAnnounceBackground: a quit stays silent once announced", "[background]") {
    REQUIRE_FALSE(shouldAnnounceBackground(WindowCloseAction::Quit, true));
}
