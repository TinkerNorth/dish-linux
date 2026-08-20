// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "core/reducer/UsbPathMachine.h"

#include <catch2/catch_test_macros.hpp>

using dish::reducer::DirectClaimFailure;
using dish::reducer::PathChoice;
using dish::reducer::resolvePathChoice;

TEST_CASE("an explicit stored pick always wins", "[usb-pathresolution]") {
    CHECK(resolvePathChoice(PathChoice::Direct, /*isFastLaneModel=*/false,
                            /*priorFailure=*/std::nullopt) == PathChoice::Direct);
    CHECK(resolvePathChoice(PathChoice::Standard, /*isFastLaneModel=*/true,
                            /*priorFailure=*/std::nullopt) == PathChoice::Standard);
}

TEST_CASE("with no stored pick a verified fast-lane model auto-selects Direct",
          "[usb-pathresolution]") {
    CHECK(resolvePathChoice(std::nullopt, /*isFastLaneModel=*/true,
                            /*priorFailure=*/std::nullopt) == PathChoice::Direct);
}

TEST_CASE("a fast-lane model that just failed to claim is not auto-Directed",
          "[usb-pathresolution]") {
    CHECK(resolvePathChoice(std::nullopt, /*isFastLaneModel=*/true, DirectClaimFailure::Busy) ==
          PathChoice::Standard);
}

TEST_CASE("an unknown model with no stored pick defaults to Standard", "[usb-pathresolution]") {
    CHECK(resolvePathChoice(std::nullopt, /*isFastLaneModel=*/false,
                            /*priorFailure=*/std::nullopt) == PathChoice::Standard);
}
