// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#pragma once

#include <cstdint>

namespace dish::input {

// SDL → wire conversion helpers for the motion (gyro / accel) and touchpad
// streams. SDL hands these out in physical units (rad/s, m/s², 0..1); the
// Satellite wire format wants resolution-independent signed int16. Extracted
// from SDLGamepadBridge.cpp into their own translation unit so the arithmetic
// can be unit-tested without bringing up SDL or Qt.
//
// Wire scale matches satellite/src/core/types.h:
//   gyro:  SDL gives rad/s; convert to deg/s, then to int16 LSB = 2000/32767
//   accel: SDL gives m/s²;  convert to g (÷ 9.80665), then int16 LSB = 4/32767

// rad/s → wire int16. Full scale ±2000 deg/s maps to ±32767; values beyond
// that clamp.
std::int16_t gyroRadPerSecToInt16(float radPerSec);

// m/s² → wire int16. ±4 g maps to ±32767; values beyond that clamp.
std::int16_t accelMps2ToInt16(float mps2);

// SDL reports touchpad coordinates as 0..1 (top-left origin). The wire wants
// a resolution-independent signed int16 spanning the pad, so map
// 0 → -32768, 1 → +32767. Inputs outside [0,1] clamp.
std::int16_t touchpadCoordToInt16(float v);

} // namespace dish::input
