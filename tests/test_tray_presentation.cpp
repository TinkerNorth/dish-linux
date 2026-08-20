// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The presentation is also the distinct-until-changed key for the tray
// Observable, so its equality is load-bearing: a field dropped from operator==
// stops the item from ever being redrawn for that field, silently.

#include "core/reducer/TrayPresentation.h"

#include <catch2/catch_test_macros.hpp>

using dish::reducer::deriveTrayPresentation;
using dish::reducer::TrayActivity;
using dish::reducer::TrayPresentation;

TEST_CASE("deriveTrayPresentation: no streaming slot reads idle", "[tray]") {
    const auto p = deriveTrayPresentation(true, 0);
    REQUIRE(p.activity == TrayActivity::Idle);
    REQUIRE(p.streamingSlots == 0);
}

TEST_CASE("deriveTrayPresentation: one or more slots read streaming", "[tray]") {
    REQUIRE(deriveTrayPresentation(true, 1).activity == TrayActivity::Streaming);
    REQUIRE(deriveTrayPresentation(true, 4).activity == TrayActivity::Streaming);
}

TEST_CASE("deriveTrayPresentation: the streaming count is carried through", "[tray]") {
    // The count is what the tooltip renders, so it travels with the activity
    // rather than being recomputed at the panel.
    REQUIRE(deriveTrayPresentation(true, 3).streamingSlots == 3);
}

TEST_CASE("deriveTrayPresentation: a negative count clamps to zero and reads idle", "[tray]") {
    // Nothing should hand this a negative, but a clamped count keeps a bad
    // upstream from rendering "-1 controllers streaming" on the panel.
    const auto p = deriveTrayPresentation(true, -2);
    REQUIRE(p.streamingSlots == 0);
    REQUIRE(p.activity == TrayActivity::Idle);
}

TEST_CASE("deriveTrayPresentation: window visibility is carried through both ways", "[tray]") {
    REQUIRE(deriveTrayPresentation(true, 0).windowVisible);
    REQUIRE_FALSE(deriveTrayPresentation(false, 0).windowVisible);
    REQUIRE(deriveTrayPresentation(true, 2).windowVisible);
    REQUIRE_FALSE(deriveTrayPresentation(false, 2).windowVisible);
}

TEST_CASE("TrayPresentation: equal fields compare equal", "[tray]") {
    const TrayPresentation a{TrayActivity::Streaming, 2, true};
    const TrayPresentation b{TrayActivity::Streaming, 2, true};
    REQUIRE(a == b);
    REQUIRE_FALSE(a != b);
}

TEST_CASE("TrayPresentation: a differing activity compares unequal", "[tray]") {
    const TrayPresentation a{TrayActivity::Streaming, 2, true};
    const TrayPresentation b{TrayActivity::Idle, 2, true};
    REQUIRE(a != b);
}

TEST_CASE("TrayPresentation: a differing streaming count compares unequal", "[tray]") {
    const TrayPresentation a{TrayActivity::Streaming, 2, true};
    const TrayPresentation b{TrayActivity::Streaming, 3, true};
    REQUIRE(a != b);
}

TEST_CASE("TrayPresentation: a differing window visibility compares unequal", "[tray]") {
    const TrayPresentation a{TrayActivity::Streaming, 2, true};
    const TrayPresentation b{TrayActivity::Streaming, 2, false};
    REQUIRE(a != b);
}
