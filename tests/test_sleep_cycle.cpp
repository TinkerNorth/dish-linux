// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// logind repeats PrepareForSleep(true) when a suspend escalates to hibernate,
// and a resume can arrive for a suspend Dish never saw, so the two properties
// that matter are about the edges that must NOT act.

#include "core/reducer/SleepCycle.h"

#include <catch2/catch_test_macros.hpp>

using dish::reducer::reduceSleepCycle;
using dish::reducer::SleepEffect;
using dish::reducer::SleepPhase;
using dish::reducer::SleepReduction;

TEST_CASE("reduceSleepCycle: awake plus a suspend tears the sessions down", "[sleep]") {
    REQUIRE(reduceSleepCycle(SleepPhase::Awake, true) ==
            SleepReduction{SleepPhase::Suspending, SleepEffect::TearDown});
}

TEST_CASE("reduceSleepCycle: suspending plus another suspend does nothing", "[sleep]") {
    REQUIRE(reduceSleepCycle(SleepPhase::Suspending, true) ==
            SleepReduction{SleepPhase::Suspending, SleepEffect::None});
}

TEST_CASE("reduceSleepCycle: suspending plus a resume reconnects", "[sleep]") {
    REQUIRE(reduceSleepCycle(SleepPhase::Suspending, false) ==
            SleepReduction{SleepPhase::Awake, SleepEffect::Reconnect});
}

TEST_CASE("reduceSleepCycle: awake plus a resume does nothing", "[sleep]") {
    REQUIRE(reduceSleepCycle(SleepPhase::Awake, false) ==
            SleepReduction{SleepPhase::Awake, SleepEffect::None});
}

TEST_CASE("reduceSleepCycle: a repeated suspend does not tear down twice", "[sleep]") {
    // A suspend escalating to hibernate repeats PrepareForSleep(true). The
    // second edge must be inert: the sessions are already gone, and tearing
    // them down again would fire the reconnect bookkeeping out of order.
    const auto first = reduceSleepCycle(SleepPhase::Awake, true);
    REQUIRE(first.effect == SleepEffect::TearDown);
    const auto second = reduceSleepCycle(first.next, true);
    REQUIRE(second.effect == SleepEffect::None);
    REQUIRE(second.next == SleepPhase::Suspending);
}

TEST_CASE("reduceSleepCycle: a resume for a suspend never seen does not reconnect", "[sleep]") {
    // Dish can start after the suspend edge, or miss it on a bus reconnect.
    // Reconnecting sessions that were never torn down would double them.
    const auto r = reduceSleepCycle(SleepPhase::Awake, false);
    REQUIRE(r.effect == SleepEffect::None);
    REQUIRE(r.next == SleepPhase::Awake);
}

TEST_CASE("reduceSleepCycle: a full cycle returns to awake", "[sleep]") {
    const auto down = reduceSleepCycle(SleepPhase::Awake, true);
    REQUIRE(down.next == SleepPhase::Suspending);
    const auto up = reduceSleepCycle(down.next, false);
    REQUIRE(up.next == SleepPhase::Awake);
    REQUIRE(up.effect == SleepEffect::Reconnect);
}

TEST_CASE("SleepReduction: compares field-wise", "[sleep]") {
    const SleepReduction a{SleepPhase::Suspending, SleepEffect::TearDown};
    REQUIRE(a == SleepReduction{SleepPhase::Suspending, SleepEffect::TearDown});
    REQUIRE(a != SleepReduction{SleepPhase::Awake, SleepEffect::TearDown});
    REQUIRE(a != SleepReduction{SleepPhase::Suspending, SleepEffect::None});
}
