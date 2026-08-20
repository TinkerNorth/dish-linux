// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#pragma once

#include <cstdint>

namespace dish::input {

// SDL → wire conversion for the motion and touchpad surfaces, split out of
// SDLGamepadBridge so the arithmetic is testable without bringing up SDL.
// Scales match satellite/src/core/types.h; all three saturate at the int16
// range rather than wrapping.

// Full scale ±2000 deg/s → ±32767.
std::int16_t gyroRadPerSecToInt16(float radPerSec);

// Full scale ±4 g → ±32767 (1 g ≈ 8192).
std::int16_t accelMps2ToInt16(float mps2);

// SDL's 0..1 top-left origin → the resolution-independent centre-origin wire
// frame: 0 → -32768, 1 → +32767.
std::int16_t touchpadCoordToInt16(float v);

} // namespace dish::input
