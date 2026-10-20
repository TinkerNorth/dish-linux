// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// mapGamepad: a game controller as SDL reads it (stick Y positive down,
// triggers 0..32767, buttons already in wire bits) becomes the wire report
// (stick Y positive up, triggers 0..255).

#include "Input/JoystickMapping.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

using dish::input::GamepadSnapshot;
using dish::input::mapGamepad;
using B = dish::input::GamepadInputProcessor::Buttons;

namespace {

constexpr std::int16_t kSdlFullUp = -32768;
constexpr std::int16_t kSdlFullDown = 32767;
constexpr std::int16_t kWireFullUp = 32767;
constexpr std::int16_t kWireFullDown = -32767;

constexpr std::int16_t kSdlTriggerReleased = 0;
constexpr std::int16_t kSdlTriggerFullPull = 32767;
constexpr std::uint8_t kWireTriggerFullPull = 255;

} // namespace

TEST_CASE("a game controller held full up reads full up on both sticks", "[gamepadmap]") {
    GamepadSnapshot snap{};
    snap.leftY = kSdlFullUp;
    snap.rightY = kSdlFullUp;
    const auto st = mapGamepad(snap);
    REQUIRE(st.ly == kWireFullUp);
    REQUIRE(st.ry == kWireFullUp);
}

TEST_CASE("a game controller held full down reads full down on both sticks", "[gamepadmap]") {
    GamepadSnapshot snap{};
    snap.leftY = kSdlFullDown;
    snap.rightY = kSdlFullDown;
    const auto st = mapGamepad(snap);
    REQUIRE(st.ly == kWireFullDown);
    REQUIRE(st.ry == kWireFullDown);
}

TEST_CASE("a centred game controller reads centre on both sticks", "[gamepadmap]") {
    const auto st = mapGamepad(GamepadSnapshot{});
    REQUIRE(st.lx == 0);
    REQUIRE(st.ly == 0);
    REQUIRE(st.rx == 0);
    REQUIRE(st.ry == 0);
}

TEST_CASE("a game controller's X axes pass through untouched, the floor included", "[gamepadmap]") {
    GamepadSnapshot snap{};
    snap.leftX = -32768;
    snap.rightX = 32767;
    const auto st = mapGamepad(snap);
    REQUIRE(st.lx == -32768);
    REQUIRE(st.rx == 32767);
}

TEST_CASE("a released trigger reads zero and a full pull reads full scale", "[gamepadmap]") {
    GamepadSnapshot snap{};
    snap.leftTrigger = kSdlTriggerReleased;
    snap.rightTrigger = kSdlTriggerFullPull;
    const auto st = mapGamepad(snap);
    REQUIRE(st.lt == 0);
    REQUIRE(st.rt == kWireTriggerFullPull);
}

TEST_CASE("a negative trigger reading floors at zero", "[gamepadmap]") {
    GamepadSnapshot snap{};
    snap.leftTrigger = -32768;
    snap.rightTrigger = -1;
    const auto st = mapGamepad(snap);
    REQUIRE(st.lt == 0);
    REQUIRE(st.rt == 0);
}

TEST_CASE("a game controller's buttons pass through in wire bits", "[gamepadmap]") {
    constexpr std::uint16_t kHeld = B::kA | B::kY | B::kDpadLeft | B::kRightShoulder | B::kStart;
    GamepadSnapshot snap{};
    snap.wButtons = kHeld;
    const auto st = mapGamepad(snap);
    REQUIRE(st.wButtons == kHeld);
}
