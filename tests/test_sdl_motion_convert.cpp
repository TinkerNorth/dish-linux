// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

// Wire scale (satellite/src/core/types.h): gyro full scale +/-2000 deg/s and
// accel +/-4 g both map onto +/-32767. The conversions run in float, but every
// anchor below lands on an exact integer after std::lround.

#include "Input/SdlMotionConvert.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

using dish::input::accelMps2ToInt16;
using dish::input::gyroRadPerSecToInt16;
using dish::input::touchpadCoordToInt16;

TEST_CASE("gyroRadPerSecToInt16 maps +2000 deg/s to full positive scale", "[motionconvert]") {
    // 2000 deg/s = 34.9066 rad/s.
    REQUIRE(gyroRadPerSecToInt16(34.9066f) == 32767);
}

TEST_CASE("gyroRadPerSecToInt16 maps -2000 deg/s to negative full scale", "[motionconvert]") {
    // clampInt16's floor is -32768, but -2000 deg/s lands at raw -32767.01, so
    // the expected value is -32767.
    REQUIRE(gyroRadPerSecToInt16(-34.9066f) == -32767);
}

TEST_CASE("gyroRadPerSecToInt16 maps zero to zero", "[motionconvert]") {
    REQUIRE(gyroRadPerSecToInt16(0.0f) == 0);
}

TEST_CASE("gyroRadPerSecToInt16 clamps beyond +/-2000 deg/s", "[motionconvert]") {
    REQUIRE(gyroRadPerSecToInt16(100.0f) == 32767);
    REQUIRE(gyroRadPerSecToInt16(-100.0f) == -32768);
}

TEST_CASE("accelMps2ToInt16 maps +1 g to one quarter of full scale", "[motionconvert]") {
    // +1 g = 9.80665 m/s², a quarter of the +/-4 g full scale.
    REQUIRE(accelMps2ToInt16(9.80665f) == 8192);
}

TEST_CASE("accelMps2ToInt16 maps +4 g to full positive scale", "[motionconvert]") {
    // +4 g = 4 * 9.80665 = 39.2266 m/s².
    REQUIRE(accelMps2ToInt16(39.2266f) == 32767);
}

TEST_CASE("accelMps2ToInt16 maps zero to zero", "[motionconvert]") {
    REQUIRE(accelMps2ToInt16(0.0f) == 0);
}

TEST_CASE("accelMps2ToInt16 clamps beyond +/-4 g", "[motionconvert]") {
    REQUIRE(accelMps2ToInt16(100.0f) == 32767);
    REQUIRE(accelMps2ToInt16(-100.0f) == -32768);
}

TEST_CASE("touchpadCoordToInt16 maps the 0..1 SDL range across the int16 span", "[motionconvert]") {
    REQUIRE(touchpadCoordToInt16(0.0f) == -32768);
    REQUIRE(touchpadCoordToInt16(1.0f) == 32767);
    // 0.5 * 65535 - 32768 = -0.5, and std::lround rounds half away from zero,
    // so the centre of the pad lands on -1, not the intuitive 0.
    REQUIRE(touchpadCoordToInt16(0.5f) == -1);
}

TEST_CASE("touchpadCoordToInt16 clamps coordinates outside 0..1", "[motionconvert]") {
    REQUIRE(touchpadCoordToInt16(-1.0f) == -32768);
    REQUIRE(touchpadCoordToInt16(2.0f) == 32767);
}
