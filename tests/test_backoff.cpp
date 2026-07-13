// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The exponential reconnect-backoff schedule (contract/android/dish-windows
// parity: 1s, 2s, 4s, … capped at 60s) and the send-counter re-push guard.
// Pure — no clock, no timer.

#include "Network/Backoff.h"
#include "Network/Reconcile.h"

#include <catch2/catch_test_macros.hpp>

namespace reducer = dish::reducer;

TEST_CASE("backoffDelayMs is exponential, capped at 60s", "[backoff]") {
    REQUIRE(reducer::backoffDelayMs(1) == 1000);  // 1s
    REQUIRE(reducer::backoffDelayMs(2) == 2000);  // 2s
    REQUIRE(reducer::backoffDelayMs(3) == 4000);  // 4s
    REQUIRE(reducer::backoffDelayMs(4) == 8000);  // 8s
    REQUIRE(reducer::backoffDelayMs(5) == 16000); // 16s
    REQUIRE(reducer::backoffDelayMs(6) == 32000); // 32s
    REQUIRE(reducer::backoffDelayMs(7) == 60000); // 1000<<6 = 64000 → capped 60s
    REQUIRE(reducer::backoffDelayMs(8) == 60000); // stays capped
    REQUIRE(reducer::backoffDelayMs(100) == 60000);
}

TEST_CASE("backoffDelayMs treats a non-positive attempt as the first", "[backoff]") {
    REQUIRE(reducer::backoffDelayMs(0) == 1000);
    REQUIRE(reducer::backoffDelayMs(-5) == 1000);
}

TEST_CASE("counterNeedsRepush fires once the send counter crosses 0xF0000000", "[backoff]") {
    REQUIRE_FALSE(reducer::counterNeedsRepush(1));
    REQUIRE_FALSE(reducer::counterNeedsRepush(0xEFFFFFFFu));
    REQUIRE(reducer::counterNeedsRepush(0xF0000000u));
    REQUIRE(reducer::counterNeedsRepush(0xFFFFFFFFu));
}
