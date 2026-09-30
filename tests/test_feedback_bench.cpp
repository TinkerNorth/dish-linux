// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "core/reducer/FeedbackBench.h"

#include <catch2/catch_test_macros.hpp>

using dish::reducer::BuzzMotor;
using dish::reducer::kTestBuzzMagnitude;
using dish::reducer::kTestBuzzMs;
using dish::reducer::TestBuzz;
using dish::reducer::testBuzzFor;

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
