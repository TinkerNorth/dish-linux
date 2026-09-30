// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "core/reducer/FeedbackBench.h"

#include <catch2/catch_test_macros.hpp>

using dish::reducer::BuzzMotor;
using dish::reducer::kTestBuzzMagnitude;
using dish::reducer::kTestBuzzMs;
using dish::reducer::TestBuzz;
using dish::reducer::testBuzzFor;
using dish::reducer::TestBuzzTickets;

TEST_CASE("the weak test buzz runs only the weak motor", "[diagnostics][bench]") {
    REQUIRE(testBuzzFor(BuzzMotor::Weak) == TestBuzz{0, kTestBuzzMagnitude, kTestBuzzMs});
}

TEST_CASE("the strong test buzz runs only the strong motor", "[diagnostics][bench]") {
    REQUIRE(testBuzzFor(BuzzMotor::Strong) == TestBuzz{kTestBuzzMagnitude, 0, kTestBuzzMs});
}

TEST_CASE("the combined test buzz runs both motors at once", "[diagnostics][bench]") {
    REQUIRE(testBuzzFor(BuzzMotor::Both) ==
            TestBuzz{kTestBuzzMagnitude, kTestBuzzMagnitude, kTestBuzzMs});
}

TEST_CASE("a test buzz is dish-android's: 48000 for 400 ms", "[diagnostics][bench]") {
    REQUIRE(kTestBuzzMagnitude == 48000);
    REQUIRE(kTestBuzzMs == 400);
}

TEST_CASE("a buzz's stop ends it while no later buzz has started", "[diagnostics][bench]") {
    TestBuzzTickets tickets;
    const auto only = tickets.issue("sdl:1");
    CHECK(tickets.isLatest("sdl:1", only));
}

TEST_CASE("a later buzz on the pad leaves the earlier buzz's stop with nothing to end",
          "[diagnostics][bench]") {
    TestBuzzTickets tickets;
    const auto first = tickets.issue("sdl:1");
    const auto second = tickets.issue("sdl:1");
    CHECK_FALSE(tickets.isLatest("sdl:1", first));
    CHECK(tickets.isLatest("sdl:1", second));
}

TEST_CASE("a buzz on another pad leaves this pad's stop standing", "[diagnostics][bench]") {
    TestBuzzTickets tickets;
    const auto mine = tickets.issue("sdl:1");
    tickets.issue("sdl:2");
    CHECK(tickets.isLatest("sdl:1", mine));
}

TEST_CASE("a pad that never buzzed has no stop to honour", "[diagnostics][bench]") {
    TestBuzzTickets tickets;
    tickets.issue("sdl:2");
    CHECK_FALSE(tickets.isLatest("sdl:1", 1));
}
