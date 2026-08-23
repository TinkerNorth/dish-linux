// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The derive half of the wake subsystem: three upstreams (streaming count,
// controller activity, preferences) folded into one reach. Pins that every
// upstream really is wired in — a composer that dropped one would still compile
// and would silently pin the machine awake — and that WakeState's ==-based
// distinct-until-changed is what stops the inhibitor thrashing.

#include "composer/WakeStateComposer.h"

#include "ComposerProbe.h"

#include <catch2/catch_test_macros.hpp>

using dish::arch::Observable;
using dish::composer::deriveWakeState;
using dish::composer::WakeState;
using dish::composer::WakeStateComposer;
using dish::reducer::KeepAwakeMode;
using dish::reducer::KeepAwakePreferences;
using dish::reducer::KeepAwakeReach;
using dish::test::ComposerProbe;

namespace {

KeepAwakePreferences prefs(KeepAwakeMode mode, bool display = false) {
    KeepAwakePreferences p;
    p.mode = mode;
    p.keepDisplayAwake = display;
    return p;
}

} // namespace

TEST_CASE("deriveWakeState: zero streaming slots never holds the machine", "[wake]") {
    REQUIRE(deriveWakeState(0, false, prefs(KeepAwakeMode::WhileConnected)).reach ==
            KeepAwakeReach::None);
    REQUIRE(deriveWakeState(0, true, prefs(KeepAwakeMode::WhileConnected)).reach ==
            KeepAwakeReach::None);
}

TEST_CASE("deriveWakeState: a positive streaming count holds while connected", "[wake]") {
    REQUIRE(deriveWakeState(1, false, prefs(KeepAwakeMode::WhileConnected)).reach ==
            KeepAwakeReach::System);
    REQUIRE(deriveWakeState(3, false, prefs(KeepAwakeMode::WhileConnected)).reach ==
            KeepAwakeReach::System);
}

TEST_CASE("deriveWakeState: mode Off never holds, whatever else is true", "[wake]") {
    REQUIRE(deriveWakeState(3, true, prefs(KeepAwakeMode::Off, true)).reach ==
            KeepAwakeReach::None);
}

TEST_CASE("deriveWakeState: the timed mode follows controller activity", "[wake]") {
    REQUIRE(deriveWakeState(1, true, prefs(KeepAwakeMode::WhileControllerActive)).reach ==
            KeepAwakeReach::System);
    REQUIRE(deriveWakeState(1, false, prefs(KeepAwakeMode::WhileControllerActive)).reach ==
            KeepAwakeReach::None);
}

TEST_CASE("deriveWakeState: the display opt-in widens the reach, never creates one", "[wake]") {
    REQUIRE(deriveWakeState(1, true, prefs(KeepAwakeMode::WhileConnected, true)).reach ==
            KeepAwakeReach::SystemAndDisplay);
    // The opt-in cannot resurrect a hold the mode or the count already refused.
    REQUIRE(deriveWakeState(0, true, prefs(KeepAwakeMode::WhileConnected, true)).reach ==
            KeepAwakeReach::None);
    REQUIRE(deriveWakeState(1, false, prefs(KeepAwakeMode::WhileControllerActive, true)).reach ==
            KeepAwakeReach::None);
}

TEST_CASE("deriveWakeState: the streaming count rides along in the value", "[wake]") {
    REQUIRE(deriveWakeState(4, true, prefs(KeepAwakeMode::WhileConnected)).streamingSlotCount == 4);
}

TEST_CASE("WakeStateComposer: eager initial reflects the current inputs", "[wake]") {
    Observable<int> count(0);
    Observable<bool> active(false);
    Observable<KeepAwakePreferences> preferences{KeepAwakePreferences{}};
    WakeStateComposer composer(count, active, preferences);
    REQUIRE(composer.state().value().reach == KeepAwakeReach::None);
}

TEST_CASE("WakeStateComposer: recomputes on a streaming-count change", "[wake]") {
    Observable<int> count(0);
    Observable<bool> active(true);
    Observable<KeepAwakePreferences> preferences{KeepAwakePreferences{}};
    WakeStateComposer composer(count, active, preferences);
    ComposerProbe<WakeState> probe(composer.state());

    count.set(2);
    REQUIRE(composer.state().value().reach == KeepAwakeReach::System);
    REQUIRE(composer.state().value().streamingSlotCount == 2);

    count.set(0);
    REQUIRE(composer.state().value().reach == KeepAwakeReach::None);
    // eager initial (0) + ->2 + ->0
    REQUIRE(probe.count() == 3);
}

TEST_CASE("WakeStateComposer: recomputes on a controller-activity change", "[wake]") {
    Observable<int> count(1);
    Observable<bool> active(false);
    Observable<KeepAwakePreferences> preferences{KeepAwakePreferences{}};
    WakeStateComposer composer(count, active, preferences);
    REQUIRE(composer.state().value().reach == KeepAwakeReach::None);

    active.set(true);
    REQUIRE(composer.state().value().reach == KeepAwakeReach::System);
    active.set(false);
    REQUIRE(composer.state().value().reach == KeepAwakeReach::None);
}

TEST_CASE("WakeStateComposer: recomputes on a preference change", "[wake]") {
    Observable<int> count(1);
    Observable<bool> active(false);
    Observable<KeepAwakePreferences> preferences{KeepAwakePreferences{}};
    WakeStateComposer composer(count, active, preferences);
    REQUIRE(composer.state().value().reach == KeepAwakeReach::None); // timed mode, idle pad

    preferences.set(prefs(KeepAwakeMode::WhileConnected));
    REQUIRE(composer.state().value().reach == KeepAwakeReach::System);

    preferences.set(prefs(KeepAwakeMode::WhileConnected, true));
    REQUIRE(composer.state().value().reach == KeepAwakeReach::SystemAndDisplay);

    preferences.set(prefs(KeepAwakeMode::Off, true));
    REQUIRE(composer.state().value().reach == KeepAwakeReach::None);
}

TEST_CASE("WakeStateComposer: distinct-until-changed suppresses same-value re-emits", "[wake]") {
    Observable<int> count(0);
    Observable<bool> active(true);
    Observable<KeepAwakePreferences> preferences{KeepAwakePreferences{}};
    WakeStateComposer composer(count, active, preferences);

    count.set(1);
    ComposerProbe<WakeState> probe(composer.state()); // starts after going positive
    // The reach stays System across 1 -> 2 -> 3, but the count is part of the
    // value, so the WakeState still differs and re-emits.
    count.set(2);
    count.set(3);
    REQUIRE(probe.count() == 3); // initial(1) + 2 + 3

    const auto before = probe.count();
    count.set(3);
    REQUIRE(probe.count() == before);
}

TEST_CASE("WakeStateComposer: a preference change that does not move the reach is quiet",
          "[wake]") {
    Observable<int> count(0);
    Observable<bool> active(false);
    Observable<KeepAwakePreferences> preferences{KeepAwakePreferences{}};
    WakeStateComposer composer(count, active, preferences);
    ComposerProbe<WakeState> probe(composer.state());

    // Nothing is streaming, so the timeout is irrelevant to the derived reach.
    KeepAwakePreferences next;
    next.idleTimeoutMinutes = 90;
    preferences.set(next);
    REQUIRE(probe.count() == 1);
}
