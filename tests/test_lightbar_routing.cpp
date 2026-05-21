// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

// Coverage for dish::LightbarRouting — the pure decision layer behind the
// light bar (Task 1.4). AppModel's lightbar handler calls this function; it
// is pure (decoded message + gate bool -> optional colour) so the routing
// rule is unit-testable without the SDL / network stack.
//
// The rule under test:
//   * a MSG_LIGHTBAR colour is applied when "Follow game" is on,
//   * "Off" suppresses the colour.

#include "LightbarRouting.h"

#include "Network/SatelliteClient.h"

#include <catch2/catch_test_macros.hpp>

using dish::LightbarColor;
using dish::lightbarColorFromLightbarMessage;
using LightbarMessage = dish::net::SatelliteClient::LightbarMessage;

namespace {

LightbarMessage makeLightbar(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    LightbarMessage lm;
    lm.controllerIndex = 0;
    lm.r = r;
    lm.g = g;
    lm.b = b;
    return lm;
}

} // namespace

TEST_CASE("MSG_LIGHTBAR colour is applied when the light bar follows the game",
          "[lightbar][routing]") {
    const auto color =
        lightbarColorFromLightbarMessage(makeLightbar(0x12, 0x34, 0x56), /*followGame=*/true);
    REQUIRE(color.has_value());
    REQUIRE(*color == LightbarColor{0x12, 0x34, 0x56});
}

TEST_CASE("MSG_LIGHTBAR colour is suppressed when the light bar is Off", "[lightbar][routing]") {
    const auto color =
        lightbarColorFromLightbarMessage(makeLightbar(0xFF, 0xFF, 0xFF), /*followGame=*/false);
    REQUIRE_FALSE(color.has_value());
}

TEST_CASE("routing carries the full 0..255 channel range", "[lightbar][routing]") {
    SECTION("all zero") {
        const auto c = lightbarColorFromLightbarMessage(makeLightbar(0, 0, 0), true);
        REQUIRE(c.has_value());
        REQUIRE(*c == LightbarColor{0, 0, 0});
    }
    SECTION("all max") {
        const auto c = lightbarColorFromLightbarMessage(makeLightbar(0xFF, 0xFF, 0xFF), true);
        REQUIRE(c.has_value());
        REQUIRE(*c == LightbarColor{0xFF, 0xFF, 0xFF});
    }
}
