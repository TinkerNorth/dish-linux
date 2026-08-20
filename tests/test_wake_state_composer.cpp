// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "composer/WakeStateComposer.h"

#include "ComposerProbe.h"

#include <catch2/catch_test_macros.hpp>

using dish::arch::Observable;
using dish::composer::deriveWakeState;
using dish::composer::WakeState;
using dish::composer::WakeStateComposer;
using dish::test::ComposerProbe;

TEST_CASE("deriveWakeState: zero count and no override does not inhibit", "[wake]") {
    REQUIRE_FALSE(deriveWakeState(0, 0).shouldInhibit);
}

TEST_CASE("deriveWakeState: a positive streaming count inhibits", "[wake]") {
    REQUIRE(deriveWakeState(1, 0).shouldInhibit);
    REQUIRE(deriveWakeState(3, 0).shouldInhibit);
}

TEST_CASE("deriveWakeState: a keep-screen-on override inhibits even at zero count", "[wake]") {
    REQUIRE(deriveWakeState(0, 1).shouldInhibit);
}

TEST_CASE("WakeStateComposer: eager initial reflects the current inputs", "[wake]") {
    Observable<int> count(0);
    Observable<int> keepOn(0);
    WakeStateComposer composer(count, keepOn);
    REQUIRE_FALSE(composer.state().value().shouldInhibit);
}

TEST_CASE("WakeStateComposer: recomputes on a streaming-count change", "[wake]") {
    Observable<int> count(0);
    Observable<int> keepOn(0);
    WakeStateComposer composer(count, keepOn);
    ComposerProbe<WakeState> probe(composer.state());

    count.set(2);
    REQUIRE(composer.state().value().shouldInhibit);
    REQUIRE(composer.state().value().streamingSlotCount == 2);

    count.set(0);
    REQUIRE_FALSE(composer.state().value().shouldInhibit);
    // eager initial (0) + ->2 + ->0
    REQUIRE(probe.count() == 3);
}

TEST_CASE("WakeStateComposer: distinct-until-changed suppresses same-bool re-emits", "[wake]") {
    Observable<int> count(0);
    Observable<int> keepOn(0);
    WakeStateComposer composer(count, keepOn);

    count.set(1);
    ComposerProbe<WakeState> probe(composer.state()); // starts after going positive
    // shouldInhibit stays true across 1 -> 2 -> 3, but the count is part of the
    // value, so the WakeState still differs and re-emits.
    count.set(2);
    count.set(3);
    REQUIRE(probe.count() == 3); // initial(1) + 2 + 3

    const auto before = probe.count();
    count.set(3);
    REQUIRE(probe.count() == before);
}
