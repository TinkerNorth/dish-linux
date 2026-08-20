// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Deadzone and axis/trigger scaling math, shared with the other Dish clients:
//   sticks   |v| <= stickFlat   -> 0
//   triggers  v  <= triggerFlat -> 0 (triggers are unsigned 0..255)
//   buttons pass through untouched
// SDL2 has no OS-level equivalent of Android's per-axis `flat`, so the bridge
// installs a per-device profile instead.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace dish::input::deadzone {

// `stickFlat` is on the int16 stick scale, `triggerFlat` on the 0..255 trigger
// scale. The bridge's default profile is ~10% stick / ~5% trigger (3277 / 13).
struct Deadzones {
    std::int16_t stickFlat = 0;
    std::uint8_t triggerFlat = 0;

    bool operator==(const Deadzones& o) const {
        return stickFlat == o.stickFlat && triggerFlat == o.triggerFlat;
    }
    bool operator!=(const Deadzones& o) const { return !(*this == o); }
};

// Widened to int32 so std::abs(INT16_MIN) does not overflow.
inline std::int16_t applyStick(std::int16_t v, std::int16_t flat) {
    if (std::abs(static_cast<std::int32_t>(v)) <= static_cast<std::int32_t>(flat)) { return 0; }
    return v;
}

inline std::uint8_t applyTrigger(std::uint8_t v, std::uint8_t flat) { return v <= flat ? 0 : v; }

// Clamps before scaling so out-of-range input saturates rather than wrapping.
inline std::int16_t scaleAxis(float v, float maxMagnitude) {
    const float clamped = std::clamp(v, -1.0F, 1.0F);
    const int scaled = static_cast<int>(clamped * maxMagnitude);
    return static_cast<std::int16_t>(
        std::clamp(scaled, static_cast<int>(INT16_MIN), static_cast<int>(INT16_MAX)));
}

inline std::uint8_t scaleTrigger(float v) {
    const float clamped = std::clamp(v, 0.0F, 1.0F);
    const int scaled = static_cast<int>(std::lround(clamped * 255.0F));
    return static_cast<std::uint8_t>(std::clamp(scaled, 0, 255));
}

} // namespace dish::input::deadzone
