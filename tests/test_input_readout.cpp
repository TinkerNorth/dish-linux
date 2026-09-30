// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "core/input/GamepadButtonLayouts.h"
#include "core/input/InputReadout.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

using Catch::Approx;
using dish::input::accelG;
using dish::input::gyroDegreesPerSecond;
using dish::input::kWireButtonBits;
using dish::input::pressedButtons;
using dish::input::stickFraction;
using dish::input::stickSampleOf;
using dish::input::touchFraction;
using dish::input::triggerFraction;
using dish::input::WireButton;
namespace layout = dish::input::layout;

namespace {
constexpr int kWordBits = 16;
constexpr int kEveryBit = 0xFFFF;
} // namespace

TEST_CASE("an idle report holds no button down", "[diagnostics][input]") {
    REQUIRE(pressedButtons(0).empty());
}

TEST_CASE("held buttons read in wire-bit order", "[diagnostics][input]") {
    const auto word = static_cast<std::uint16_t>(layout::kXusbB | layout::kXusbA |
                                                 layout::kXusbDpadUp | layout::kXusbMicMute);
    REQUIRE(pressedButtons(word) == std::vector<WireButton>{WireButton::DpadUp, WireButton::MicMute,
                                                            WireButton::A, WireButton::B});
}

TEST_CASE("every bit of the XUSB word names exactly one button", "[diagnostics][input]") {
    int covered = 0;
    for (const auto& entry : kWireButtonBits) {
        REQUIRE((covered & entry.bit) == 0);
        covered |= entry.bit;
        const auto word = static_cast<std::uint16_t>(entry.bit);
        REQUIRE(pressedButtons(word) == std::vector<WireButton>{entry.button});
    }
    REQUIRE(covered == kEveryBit);
    REQUIRE(static_cast<int>(kWireButtonBits.size()) == kWordBits);
}

TEST_CASE("a stick reads its deflection as a fraction of full travel", "[diagnostics][input]") {
    REQUIRE(stickFraction(0) == 0.0F);
    REQUIRE(stickFraction(-32768) == -1.0F);
    REQUIRE(stickFraction(16384) == 0.5F);
    REQUIRE(stickFraction(32767) == Approx(1.0F).epsilon(0.0001));
}

TEST_CASE("a stick sample reads both of its axes as fractions", "[diagnostics][input]") {
    const auto sample = stickSampleOf(16384, -32768);
    REQUIRE(sample.x == 0.5F);
    REQUIRE(sample.y == -1.0F);
}

TEST_CASE("a trigger reads its pull as a fraction of full travel", "[diagnostics][input]") {
    REQUIRE(triggerFraction(0) == 0.0F);
    REQUIRE(triggerFraction(255) == 1.0F);
}

TEST_CASE("motion reads in degrees per second and in g at the wire's full scale",
          "[diagnostics][input]") {
    REQUIRE(gyroDegreesPerSecond(32767) == Approx(2000.0F));
    REQUIRE(gyroDegreesPerSecond(-32767) == Approx(-2000.0F));
    REQUIRE(gyroDegreesPerSecond(0) == 0.0F);
    REQUIRE(accelG(32767) == Approx(4.0F));
    REQUIRE(accelG(8192) == Approx(1.0F).epsilon(0.001));
}

TEST_CASE("a touch coordinate reads as a fraction across the pad", "[diagnostics][input]") {
    REQUIRE(touchFraction(-32768) == 0.0F);
    REQUIRE(touchFraction(32767) == 1.0F);
    REQUIRE(touchFraction(0) == Approx(0.5F).epsilon(0.0001));
}
