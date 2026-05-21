// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "SdlMotionConvert.h"

#include <algorithm>
#include <cmath>

namespace dish::input {

namespace {

// Wire scale matching satellite/src/core/types.h.
//
//   gyro:  SDL gives rad/s; convert to deg/s, then to int16 LSB = 2000/32767
//   accel: SDL gives m/s²;  convert to g (÷ 9.80665), then to int16 LSB = 4/32767
constexpr float kRadPerSecToDegPerSec = 57.295779513f; // 180 / π
constexpr float kGyroInt16PerDegPerSec = 32767.0f / 2000.0f;
constexpr float kAccelInt16PerG = 32767.0f / 4.0f;
constexpr float kGravityMps2 = 9.80665f;

std::int16_t clampInt16(float v) {
    const float c = std::clamp(v, -32768.0f, 32767.0f);
    return static_cast<std::int16_t>(std::lround(c));
}

} // namespace

std::int16_t gyroRadPerSecToInt16(float radPerSec) {
    return clampInt16(radPerSec * kRadPerSecToDegPerSec * kGyroInt16PerDegPerSec);
}

std::int16_t accelMps2ToInt16(float mps2) {
    return clampInt16((mps2 / kGravityMps2) * kAccelInt16PerG);
}

std::int16_t touchpadCoordToInt16(float v) {
    return clampInt16(std::clamp(v, 0.0f, 1.0f) * 65535.0f - 32768.0f);
}

} // namespace dish::input
