// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "core/reducer/HostBuiltInPad.h"

#include <catch2/catch_test_macros.hpp>

#include <vector>

using dish::reducer::defaultPadIndex;
using dish::reducer::isHostBuiltInPad;

TEST_CASE("the Steam Deck's own controls are the host's built-in pad", "[host-pad]") {
    CHECK(isHostBuiltInPad(0x28DE, 0x1205));
}

TEST_CASE("a Steam Controller, an Xbox pad and an unidentified pad are not built in",
          "[host-pad]") {
    CHECK_FALSE(isHostBuiltInPad(0x28DE, 0x1102));
    CHECK_FALSE(isHostBuiltInPad(0x28DE, 0x1142));
    CHECK_FALSE(isHostBuiltInPad(0x045E, 0x0B00));
    CHECK_FALSE(isHostBuiltInPad(0, 0));
}

TEST_CASE("the wizard's default pad is the first that is not the host's own", "[host-pad]") {
    CHECK(defaultPadIndex({false}) == 0);
    CHECK(defaultPadIndex({true, false}) == 1);
    CHECK(defaultPadIndex({true, true, false}) == 2);
    CHECK(defaultPadIndex({false, true}) == 0);
}

TEST_CASE("with only the host's own controls present they are the default after all",
          "[host-pad]") {
    CHECK(defaultPadIndex({true}) == 0);
    CHECK(defaultPadIndex({true, true}) == 0);
}

TEST_CASE("no pads means no default", "[host-pad]") { CHECK(defaultPadIndex({}) == -1); }
