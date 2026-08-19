// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The wire scale matches Input/SdlMotionConvert: gyro +/-2000 deg/s, accel +/-4 g.

#include "core/input/PhysicalMotionSource.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <map>
#include <string>

namespace motion = dish::input::motion;

namespace {

constexpr float kPi = 3.14159265358979323846f;
float degToRad(float deg) { return deg * kPi / 180.0f; }

// A stand-in connection handle: filterByCapability is templated on it.
using Conn = int;

} // namespace

TEST_CASE("zero gyro maps to zero", "[physical-motion]") {
    const auto s = motion::convertControllerSample(0.0f, 0.0f, 0.0f, 0, 0, 0);
    CHECK(s.gyroX == 0);
    CHECK(s.gyroY == 0);
    CHECK(s.gyroZ == 0);
}

TEST_CASE("gyro full scale maps to int16 max", "[physical-motion]") {
    const float fullScaleRad = degToRad(2000.0f);
    const auto s = motion::convertControllerSample(fullScaleRad, 0.0f, 0.0f, 0, 0, 0);
    CHECK(s.gyroX >= 32766);
    CHECK(s.gyroX <= 32767);
}

TEST_CASE("gyro axes are NOT remapped - identity, unlike the phone path", "[physical-motion]") {
    const float gx = degToRad(200.0f);
    const float gy = degToRad(-600.0f);
    const float gz = degToRad(1000.0f);
    const auto s = motion::convertControllerSample(gx, gy, gz, 0, 0, 0);
    CHECK(s.gyroX == dish::input::gyroRadPerSecToInt16(gx));
    CHECK(s.gyroY == dish::input::gyroRadPerSecToInt16(gy));
    CHECK(s.gyroZ == dish::input::gyroRadPerSecToInt16(gz));
    CHECK(s.gyroX > 0);
    CHECK(s.gyroY < 0);
    CHECK(s.gyroZ > 0);
}

TEST_CASE("accel triple passes through already-scaled", "[physical-motion]") {
    const auto s = motion::convertControllerSample(0.0f, 0.0f, 0.0f, 1234, -5678, 8191);
    CHECK(s.accelX == 1234);
    CHECK(s.accelY == -5678);
    CHECK(s.accelZ == 8191);
}

TEST_CASE("gyro beyond full scale clamps to the int16 range", "[physical-motion]") {
    const float overRad = degToRad(9000.0f);
    const auto s = motion::convertControllerSample(overRad, -overRad, 0.0f, 0, 0, 0);
    CHECK(s.gyroX == 32767);
    CHECK(s.gyroY == -32768);
}

TEST_CASE("shouldEmitGyro is true when the pad has no accelerometer", "[physical-motion]") {
    CHECK(motion::shouldEmitGyro(/*hasAccelSensor=*/false, /*accelSeen=*/false));
}

TEST_CASE("shouldEmitGyro is false on the first gyro before accel reports", "[physical-motion]") {
    CHECK_FALSE(motion::shouldEmitGyro(/*hasAccelSensor=*/true, /*accelSeen=*/false));
}

TEST_CASE("shouldEmitGyro is true once accel has reported", "[physical-motion]") {
    CHECK(motion::shouldEmitGyro(/*hasAccelSensor=*/true, /*accelSeen=*/true));
}

TEST_CASE("shouldEmitGyro accel-absent path ignores accelSeen", "[physical-motion]") {
    CHECK(motion::shouldEmitGyro(/*hasAccelSensor=*/false, /*accelSeen=*/true));
}

TEST_CASE("filterByCapability keeps a reachable slot with gyro AND enabled", "[physical-motion]") {
    const std::map<std::string, Conn> reachable{{"9", 1}};
    const std::map<std::string, motion::MotionGate> gates{{"9", {true, true}}};
    CHECK(motion::filterByCapability(reachable, gates) == reachable);
}

TEST_CASE("filterByCapability drops a reachable slot whose pad has no gyro", "[physical-motion]") {
    const std::map<std::string, Conn> reachable{{"9", 1}};
    const std::map<std::string, motion::MotionGate> gates{{"9", {false, true}}};
    CHECK(motion::filterByCapability(reachable, gates).empty());
}

TEST_CASE("filterByCapability drops a slot the user toggled motion off for", "[physical-motion]") {
    const std::map<std::string, Conn> reachable{{"9", 1}};
    const std::map<std::string, motion::MotionGate> gates{{"9", {true, false}}};
    CHECK(motion::filterByCapability(reachable, gates).empty());
}

TEST_CASE("filterByCapability drops a reachable slot missing from gates", "[physical-motion]") {
    // Startup race: reachability emits before the capability map.
    const std::map<std::string, Conn> reachable{{"9", 1}};
    const std::map<std::string, motion::MotionGate> gates{};
    CHECK(motion::filterByCapability(reachable, gates).empty());
}

TEST_CASE("filterByCapability per-slot keeps the enabled one, drops the disabled one",
          "[physical-motion]") {
    const std::map<std::string, Conn> reachable{{"A", 1}, {"B", 2}};
    const std::map<std::string, motion::MotionGate> gates{{"A", {true, true}},
                                                          {"B", {true, false}}};
    const std::map<std::string, Conn> expected{{"A", 1}};
    CHECK(motion::filterByCapability(reachable, gates) == expected);
}

TEST_CASE("probeHasGyro is false when the sensor API is unavailable", "[physical-motion]") {
    CHECK_FALSE(motion::probeHasGyro(/*sensorApiAvailable=*/false, /*deviceHasGyro=*/true));
}

TEST_CASE("probeHasGyro is false when the pad reports no gyro", "[physical-motion]") {
    CHECK_FALSE(motion::probeHasGyro(/*sensorApiAvailable=*/true, /*deviceHasGyro=*/false));
}

TEST_CASE("probeHasGyro is true when the API is available and the pad has a gyro",
          "[physical-motion]") {
    CHECK(motion::probeHasGyro(/*sensorApiAvailable=*/true, /*deviceHasGyro=*/true));
}

TEST_CASE("probeHasGyro requires both the API and the device sensor", "[physical-motion]") {
    CHECK_FALSE(motion::probeHasGyro(false, false));
}
