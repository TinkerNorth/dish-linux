// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The scalar math only; the processor-level deadzone behaviour is covered by
// test_gamepad_input_processor.cpp.

#include "core/input/Deadzones.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

namespace dz = dish::input::deadzone;

TEST_CASE("applyStick zeroes at or below the flat magnitude", "[input][deadzone]") {
    REQUIRE(dz::applyStick(0, 3277) == 0);
    REQUIRE(dz::applyStick(3277, 3277) == 0);
    REQUIRE(dz::applyStick(-3277, 3277) == 0);
    REQUIRE(dz::applyStick(1500, 3277) == 0);
    REQUIRE(dz::applyStick(-2000, 3277) == 0);
}

TEST_CASE("applyStick passes values above the flat magnitude", "[input][deadzone]") {
    REQUIRE(dz::applyStick(3278, 3277) == 3278);
    REQUIRE(dz::applyStick(-3278, 3277) == -3278);
    REQUIRE(dz::applyStick(32767, 3277) == 32767);
    REQUIRE(dz::applyStick(-32767, 3277) == -32767);
}

TEST_CASE("applyStick handles INT16_MIN without overflow", "[input][deadzone]") {
    // std::abs(INT16_MIN) would overflow a 16-bit signed; the impl widens to
    // int32 so the most-negative value still passes a small flat.
    REQUIRE(dz::applyStick(INT16_MIN, 100) == INT16_MIN);
    REQUIRE(dz::applyStick(INT16_MIN, 32767) == INT16_MIN); // |−32768| > 32767 -> passes
    REQUIRE(dz::applyStick(-32767, 32767) == 0);            // |−32767| == flat -> 0
}

TEST_CASE("applyTrigger zeroes at or below the flat", "[input][deadzone]") {
    REQUIRE(dz::applyTrigger(0, 13) == 0);
    REQUIRE(dz::applyTrigger(5, 13) == 0);
    REQUIRE(dz::applyTrigger(13, 13) == 0);
}

TEST_CASE("applyTrigger passes values above the flat", "[input][deadzone]") {
    REQUIRE(dz::applyTrigger(14, 13) == 14);
    REQUIRE(dz::applyTrigger(255, 13) == 255);
    REQUIRE(dz::applyTrigger(1, 0) == 1); // zero flat passes any nonzero
}

TEST_CASE("scaleAxis clamps inputs to [-1, 1]", "[input][deadzone]") {
    REQUIRE(dz::scaleAxis(-2.0F, 32767.0F) == INT16_MIN + 1); // 32767 magnitude clamps to -32767
    REQUIRE(dz::scaleAxis(2.0F, 32767.0F) == 32767);
    REQUIRE(dz::scaleAxis(0.0F, 32767.0F) == 0);
    REQUIRE(dz::scaleAxis(0.5F, 32767.0F) == 16383);
}

TEST_CASE("scaleTrigger clamps and rounds to [0, 255]", "[input][deadzone]") {
    REQUIRE(dz::scaleTrigger(-1.0F) == 0);
    REQUIRE(dz::scaleTrigger(0.0F) == 0);
    REQUIRE(dz::scaleTrigger(0.5F) == 128);
    REQUIRE(dz::scaleTrigger(1.0F) == 255);
    REQUIRE(dz::scaleTrigger(2.0F) == 255);
}

TEST_CASE("Deadzones value type compares by both fields", "[input][deadzone]") {
    REQUIRE(dz::Deadzones{3277, 13} == dz::Deadzones{3277, 13});
    REQUIRE(dz::Deadzones{3277, 13} != dz::Deadzones{3277, 14});
    REQUIRE(dz::Deadzones{3277, 13} != dz::Deadzones{3000, 13});
}
