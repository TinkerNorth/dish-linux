// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "core/reducer/SatelliteLinkState.h"

#include <catch2/catch_test_macros.hpp>

using dish::reducer::satelliteLinkState;
using dish::reducer::SessionPresence;
using dish::reducer::UiLinkState;

TEST_CASE("satelliteLinkState: live is connected", "[linkstate]") {
    REQUIRE(satelliteLinkState(SessionPresence::Live, false, false) == UiLinkState::Connected);
}

TEST_CASE("satelliteLinkState: linking is connecting", "[linkstate]") {
    REQUIRE(satelliteLinkState(SessionPresence::Linking, false, false) == UiLinkState::Connecting);
}

TEST_CASE("satelliteLinkState: faltering is unstable", "[linkstate]") {
    REQUIRE(satelliteLinkState(SessionPresence::Faltering, false, false) == UiLinkState::Unstable);
}

TEST_CASE("satelliteLinkState: idle and stale needs pairing", "[linkstate]") {
    REQUIRE(satelliteLinkState(SessionPresence::Idle, /*isStale=*/true, false) ==
            UiLinkState::Stale);
}

TEST_CASE("satelliteLinkState: idle and discovered is ready", "[linkstate]") {
    REQUIRE(satelliteLinkState(SessionPresence::Idle, false, /*isDiscovered=*/true) ==
            UiLinkState::Ready);
}

TEST_CASE("satelliteLinkState: idle remembered-only is saved", "[linkstate]") {
    REQUIRE(satelliteLinkState(SessionPresence::Idle, false, false) == UiLinkState::Saved);
}

TEST_CASE("satelliteLinkState: stale presence is always needs pairing", "[linkstate]") {
    // A Stale presence dropped the key, so being discovered cannot rescue it.
    REQUIRE(satelliteLinkState(SessionPresence::Stale, false, false) == UiLinkState::Stale);
    REQUIRE(satelliteLinkState(SessionPresence::Stale, false, true) == UiLinkState::Stale);
}

TEST_CASE("satelliteLinkState: stale wins over discovered when idle", "[linkstate]") {
    REQUIRE(satelliteLinkState(SessionPresence::Idle, /*isStale=*/true, /*isDiscovered=*/true) ==
            UiLinkState::Stale);
}

TEST_CASE("satelliteLinkState: a connected link ignores stale and discovered flags",
          "[linkstate]") {
    REQUIRE(satelliteLinkState(SessionPresence::Live, /*isStale=*/true, /*isDiscovered=*/true) ==
            UiLinkState::Connected);
}
