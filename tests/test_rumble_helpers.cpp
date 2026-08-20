// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "core/reducer/RumbleRouting.h"

#include <catch2/catch_test_macros.hpp>

#include <climits>

using dish::reducer::rumbleMagnitudeTo255;
using dish::reducer::rumbleSafeDurationMs;

TEST_CASE("rumbleMagnitudeTo255: magnitude 0 stays 0", "[rumble][helpers]") {
    REQUIRE(rumbleMagnitudeTo255(0) == 0);
}

TEST_CASE("rumbleMagnitudeTo255: magnitude 65535 maps to 255", "[rumble][helpers]") {
    REQUIRE(rumbleMagnitudeTo255(65535) == 255);
}

TEST_CASE("rumbleMagnitudeTo255: midpoint maps to ~128 with even rounding", "[rumble][helpers]") {
    // 32767 falls in the lower half, 32768 rounds up — the +32767 bias.
    REQUIRE(rumbleMagnitudeTo255(32767) == 127);
    REQUIRE(rumbleMagnitudeTo255(32768) == 128);
}

TEST_CASE("rumbleMagnitudeTo255: tiny magnitude rounds up to 1, not down to 0",
          "[rumble][helpers]") {
    // A non-zero magnitude must never become silent — clamp UP to 1.
    REQUIRE(rumbleMagnitudeTo255(1) == 1);
    REQUIRE(rumbleMagnitudeTo255(50) == 1);
    REQUIRE(rumbleMagnitudeTo255(127) == 1);
    REQUIRE(rumbleMagnitudeTo255(128) == 1);
}

TEST_CASE("rumbleMagnitudeTo255: magnitude over 65535 is clamped to 255", "[rumble][helpers]") {
    REQUIRE(rumbleMagnitudeTo255(70000) == 255);
    REQUIRE(rumbleMagnitudeTo255(INT_MAX) == 255);
}

TEST_CASE("rumbleMagnitudeTo255: magnitude below 0 is treated as 0", "[rumble][helpers]") {
    REQUIRE(rumbleMagnitudeTo255(-1) == 0);
    REQUIRE(rumbleMagnitudeTo255(INT_MIN) == 0);
}

TEST_CASE("rumbleMagnitudeTo255: scaling is monotonic across the range", "[rumble][helpers]") {
    int prev = -1;
    for (int m = 0; m <= 65535; m += 137) {
        const int v = rumbleMagnitudeTo255(m);
        REQUIRE(v >= prev);
        prev = v;
    }
    // Pinned explicitly: the loop step can skip the endpoints.
    REQUIRE(rumbleMagnitudeTo255(0) == 0);
    REQUIRE(rumbleMagnitudeTo255(65535) == 255);
}

TEST_CASE("rumbleSafeDurationMs: duration 0 is preserved as the stop sentinel",
          "[rumble][helpers]") {
    REQUIRE(rumbleSafeDurationMs(0) == 0);
}

TEST_CASE("rumbleSafeDurationMs: in-range durations pass through unchanged", "[rumble][helpers]") {
    REQUIRE(rumbleSafeDurationMs(1) == 1);
    REQUIRE(rumbleSafeDurationMs(500) == 500);
    REQUIRE(rumbleSafeDurationMs(1500) == 1500);
}

TEST_CASE("rumbleSafeDurationMs: duration above 1500 is clamped to 1500", "[rumble][helpers]") {
    REQUIRE(rumbleSafeDurationMs(2000) == 1500);
    REQUIRE(rumbleSafeDurationMs(65535) == 1500);
    REQUIRE(rumbleSafeDurationMs(INT_MAX) == 1500);
}

TEST_CASE("rumbleSafeDurationMs: negative duration is clamped to 1", "[rumble][helpers]") {
    REQUIRE(rumbleSafeDurationMs(-1) == 1);
    REQUIRE(rumbleSafeDurationMs(INT_MIN) == 1);
}
