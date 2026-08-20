// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The decision half of the physical-controller motion path, kept free of SDL so
// it host-tests. SDL sensor registration and lifecycle stay on the bridge.

#pragma once

#include "Input/SdlMotionConvert.h"

#include <cstdint>
#include <map>
#include <string>

namespace dish::input::motion {

// The six int16 fields of an MSG_MOTION body.
struct MotionSample {
    std::int16_t gyroX = 0;
    std::int16_t gyroY = 0;
    std::int16_t gyroZ = 0;
    std::int16_t accelX = 0;
    std::int16_t accelY = 0;
    std::int16_t accelZ = 0;

    bool operator==(const MotionSample& o) const {
        return gyroX == o.gyroX && gyroY == o.gyroY && gyroZ == o.gyroZ && accelX == o.accelX &&
               accelY == o.accelY && accelZ == o.accelZ;
    }
};

// Axes are identity: a controller's IMU body frame already matches the wire
// convention, so unlike the phone path there is no landscape remap. Accel arrives
// already scaled by the caller.
inline MotionSample convertControllerSample(float gyroX, float gyroY, float gyroZ,
                                            std::int16_t accelX, std::int16_t accelY,
                                            std::int16_t accelZ) {
    MotionSample s;
    s.gyroX = gyroRadPerSecToInt16(gyroX);
    s.gyroY = gyroRadPerSecToInt16(gyroY);
    s.gyroZ = gyroRadPerSecToInt16(gyroZ);
    s.accelX = accelX;
    s.accelY = accelY;
    s.accelZ = accelZ;
    return s;
}

// A pad with an accelerometer holds its first gyro sample until accel reports, so
// it never ships an unwritten (0,0,0) accel as genuine zero gravity. A pad with
// no accelerometer emits immediately rather than waiting forever.
inline bool shouldEmitGyro(bool hasAccelSensor, bool accelSeen) {
    return !hasAccelSensor || accelSeen;
}

// Kept local so core/input does not depend on the composer.
struct MotionGate {
    bool hasGyro = false;
    bool userEnabled = false;
};

// `Conn` is templated only so core/input stays decoupled from the connection
// layer. A slot missing from `gates` is dropped: unknown means no motion, which
// is the safe default during the startup race.
template <class Conn>
std::map<std::string, Conn> filterByCapability(const std::map<std::string, Conn>& reachable,
                                               const std::map<std::string, MotionGate>& gates) {
    std::map<std::string, Conn> out;
    for (const auto& [slotId, conn] : reachable) {
        const auto it = gates.find(slotId);
        if (it == gates.end()) { continue; }
        if (it->second.hasGyro && it->second.userEnabled) { out.emplace(slotId, conn); }
    }
    return out;
}

// `sensorApiAvailable` exists for platforms that gate the sensor API by version;
// under SDL2 it is always true. `deviceHasGyro` comes from
// SDL_GameControllerHasSensor.
inline bool probeHasGyro(bool sensorApiAvailable, bool deviceHasGyro) {
    return sensorApiAvailable && deviceHasGyro;
}

} // namespace dish::input::motion
