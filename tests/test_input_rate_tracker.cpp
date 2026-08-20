// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "core/reducer/InputRateTracker.h"

#include <catch2/catch_test_macros.hpp>

using dish::reducer::InputRateTracker;
using dish::reducer::quantizeHz;

namespace {
constexpr std::uint64_t kSecondUs = 1'000'000ULL;
} // namespace

TEST_CASE("quantizeHz: rounds to the nearest multiple of 5", "[inputrate]") {
    REQUIRE(quantizeHz(0.0) == 0);
    REQUIRE(quantizeHz(2.0) == 0);   // 2 -> nearest 5 is 0
    REQUIRE(quantizeHz(3.0) == 5);   // 3 -> 5
    REQUIRE(quantizeHz(22.0) == 20); // 22 -> 20
    REQUIRE(quantizeHz(23.0) == 25); // 23 -> 25
    REQUIRE(quantizeHz(50.0) == 50); // exact boundary stays
    REQUIRE(quantizeHz(247.0) == 245);
    REQUIRE(quantizeHz(248.0) == 250);
}

TEST_CASE("quantizeHz: never negative", "[inputrate]") {
    REQUIRE(quantizeHz(-10.0) == 0);
    REQUIRE(quantizeHz(-0.4) == 0);
}

TEST_CASE("tracker: first sample reports zero and anchors", "[inputrate]") {
    InputRateTracker t;
    REQUIRE(t.sample(1000, 0) == 0);
    REQUIRE(t.lastHz() == 0);
}

TEST_CASE("tracker: 200 events in one second is 200 Hz", "[inputrate]") {
    InputRateTracker t;
    t.sample(0, 0);
    REQUIRE(t.sample(200, kSecondUs) == 200);
}

TEST_CASE("tracker: a half-second window doubles the rate", "[inputrate]") {
    InputRateTracker t;
    t.sample(0, 0);
    // 60 events over 500 ms is 120 Hz, already a multiple of 5.
    REQUIRE(t.sample(60, kSecondUs / 2) == 120);
}

TEST_CASE("tracker: a raw rate is quantized to the nearest 5 Hz", "[inputrate]") {
    InputRateTracker t;
    t.sample(0, 0);
    REQUIRE(t.sample(23, kSecondUs) == 25);
}

TEST_CASE("tracker: counter going backwards reports zero and rebaselines", "[inputrate]") {
    InputRateTracker t;
    t.sample(0, 0);
    REQUIRE(t.sample(500, kSecondUs) == 500);   // running normally
    REQUIRE(t.sample(100, 2 * kSecondUs) == 0); // counter reset -> 0
    // The next interval measures from the new anchor (100), not from 500.
    REQUIRE(t.sample(140, 3 * kSecondUs) == 40); // 40 events in 1 s = 40 Hz
}

TEST_CASE("tracker: same-instant sample holds the last rate", "[inputrate]") {
    InputRateTracker t;
    t.sample(0, 0);
    REQUIRE(t.sample(100, kSecondUs) == 100);
    // Same timestamp: hold the rate but still advance the count anchor, so the
    // next real interval measures from 150.
    REQUIRE(t.sample(150, kSecondUs) == 100);
    REQUIRE(t.sample(200, 2 * kSecondUs) == 50);
}

TEST_CASE("tracker: no events in the window reports zero", "[inputrate]") {
    InputRateTracker t;
    t.sample(1000, 0);
    REQUIRE(t.sample(1000, kSecondUs) == 0);
}

TEST_CASE("tracker: reset re-anchors so the next sample reports zero", "[inputrate]") {
    InputRateTracker t;
    t.sample(0, 0);
    REQUIRE(t.sample(100, kSecondUs) == 100);
    t.reset();
    REQUIRE(t.sample(500, 2 * kSecondUs) == 0); // fresh baseline, not 400 Hz
    REQUIRE(t.sample(550, 3 * kSecondUs) == 50);
}
