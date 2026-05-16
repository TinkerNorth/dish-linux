// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

// Coverage for the SDL → wire conversion helpers in SdlMotionConvert.{h,cpp}.
// SDL hands gyro/accel/touchpad data out in physical units; these helpers map
// them to the resolution-independent signed int16 the Satellite wire format
// expects (scale defined in satellite/src/core/types.h). The arithmetic is the
// only branching logic here worth pinning — it lives in its own TU precisely
// so it can be exercised without bringing up SDL or Qt.
//
// Reference scale:
//   gyro:  full scale ±2000 deg/s ↦ ±32767 (int16 LSB = 2000/32767 deg/s)
//   accel: full scale ±4 g        ↦ ±32767 (int16 LSB = 4/32767 g)

#include "Input/SdlMotionConvert.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

using dish::input::accelMps2ToInt16;
using dish::input::gyroRadPerSecToInt16;
using dish::input::touchpadCoordToInt16;

// ── Gyro: rad/s → wire int16 ────────────────────────────────────────────────

TEST_CASE("gyroRadPerSecToInt16 maps +2000 deg/s to full positive scale", "[motionconvert]") {
    // 2000 deg/s in rad/s = 2000 / (180/π) ≈ 34.9066. The wire's positive
    // full scale is +32767.
    REQUIRE(gyroRadPerSecToInt16(34.9066f) == 32767);
}

TEST_CASE("gyroRadPerSecToInt16 maps -2000 deg/s to negative full scale", "[motionconvert]") {
    // Negative full-scale. clampInt16's floor is -32768, but -2000 deg/s lands
    // at raw ≈ -32767.01, which clamps to -32767 (not -32768).
    REQUIRE(gyroRadPerSecToInt16(-34.9066f) == -32767);
}

TEST_CASE("gyroRadPerSecToInt16 maps zero to zero", "[motionconvert]") {
    REQUIRE(gyroRadPerSecToInt16(0.0f) == 0);
}

TEST_CASE("gyroRadPerSecToInt16 clamps beyond +/-2000 deg/s", "[motionconvert]") {
    // A pad spun far past the ±2000 deg/s range must not wrap — it pins to
    // the int16 extremes.
    REQUIRE(gyroRadPerSecToInt16(100.0f) == 32767);
    REQUIRE(gyroRadPerSecToInt16(-100.0f) == -32768);
}

// ── Accel: m/s² → wire int16 ────────────────────────────────────────────────

TEST_CASE("accelMps2ToInt16 maps +1 g to one quarter of full scale", "[motionconvert]") {
    // +1 g = 9.80665 m/s². Wire scale is ±4 g ↦ ±32767, so +1 g ≈ 8192.
    REQUIRE(accelMps2ToInt16(9.80665f) == 8192);
}

TEST_CASE("accelMps2ToInt16 maps +4 g to full positive scale", "[motionconvert]") {
    // +4 g = 4 * 9.80665 = 39.2266 m/s² ↦ +32767.
    REQUIRE(accelMps2ToInt16(39.2266f) == 32767);
}

TEST_CASE("accelMps2ToInt16 maps zero to zero", "[motionconvert]") {
    REQUIRE(accelMps2ToInt16(0.0f) == 0);
}

TEST_CASE("accelMps2ToInt16 clamps beyond +/-4 g", "[motionconvert]") {
    // A hard knock past ±4 g pins to the int16 extremes rather than wrapping.
    REQUIRE(accelMps2ToInt16(100.0f) == 32767);
    REQUIRE(accelMps2ToInt16(-100.0f) == -32768);
}

// ── Touchpad: SDL 0..1 → wire int16 ─────────────────────────────────────────

TEST_CASE("touchpadCoordToInt16 maps the 0..1 SDL range across the int16 span",
          "[motionconvert]") {
    REQUIRE(touchpadCoordToInt16(0.0f) == -32768);
    REQUIRE(touchpadCoordToInt16(1.0f) == 32767);
    // Midpoint: 0.5 * 65535 - 32768 = -0.5, and std::lround rounds half away
    // from zero, so the centre of the pad lands on -1. The int16 range
    // [-32768, 32767] has no exact midpoint at 0 — pin the real value rather
    // than the intuitive-but-wrong 0.
    REQUIRE(touchpadCoordToInt16(0.5f) == -1);
}

TEST_CASE("touchpadCoordToInt16 clamps coordinates outside 0..1", "[motionconvert]") {
    REQUIRE(touchpadCoordToInt16(-1.0f) == -32768);
    REQUIRE(touchpadCoordToInt16(2.0f) == 32767);
}
