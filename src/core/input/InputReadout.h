// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// What the input inspector shows of one wire report: which buttons the XUSB
// word holds down, and the sticks, triggers, motion and touch in human units.
// The button table reads the same layout constants the report builders write,
// so the inspector cannot name a bit the wire does not carry.

#pragma once

#include "core/input/GamepadButtonLayouts.h"
#include "core/input/StickHealth.h"

#include <array>
#include <cstdint>
#include <vector>

namespace dish::input {

enum class WireButton : std::uint8_t {
    DpadUp,
    DpadDown,
    DpadLeft,
    DpadRight,
    Start,
    Back,
    LeftThumb,
    RightThumb,
    LeftShoulder,
    RightShoulder,
    Guide,
    MicMute,
    A,
    B,
    X,
    Y,
};

struct WireButtonBit {
    int bit = 0;
    WireButton button = WireButton::DpadUp;
};

inline constexpr std::array<WireButtonBit, 16> kWireButtonBits{{
    {layout::kXusbDpadUp, WireButton::DpadUp},
    {layout::kXusbDpadDown, WireButton::DpadDown},
    {layout::kXusbDpadLeft, WireButton::DpadLeft},
    {layout::kXusbDpadRight, WireButton::DpadRight},
    {layout::kXusbStart, WireButton::Start},
    {layout::kXusbBack, WireButton::Back},
    {layout::kXusbLeftThumb, WireButton::LeftThumb},
    {layout::kXusbRightThumb, WireButton::RightThumb},
    {layout::kXusbLeftShoulder, WireButton::LeftShoulder},
    {layout::kXusbRightShoulder, WireButton::RightShoulder},
    {layout::kXusbGuide, WireButton::Guide},
    {layout::kXusbMicMute, WireButton::MicMute},
    {layout::kXusbA, WireButton::A},
    {layout::kXusbB, WireButton::B},
    {layout::kXusbX, WireButton::X},
    {layout::kXusbY, WireButton::Y},
}};

// The buttons a report's XUSB word holds down, in wire-bit order.
inline std::vector<WireButton> pressedButtons(std::uint16_t wButtons) {
    std::vector<WireButton> pressed;
    for (const auto& entry : kWireButtonBits) {
        if ((wButtons & entry.bit) != 0) { pressed.push_back(entry.button); }
    }
    return pressed;
}

// A stick axis as a fraction of full deflection, -1..1. Divided by 32768 like
// dish-android's inspector, so both clients read one sample the same.
inline constexpr float kWireStickScale = 32768.0F;

inline float stickFraction(std::int16_t axis) { return static_cast<float>(axis) / kWireStickScale; }

inline StickSample stickSampleOf(std::int16_t x, std::int16_t y) {
    return {stickFraction(x), stickFraction(y)};
}

// A trigger as a fraction of full pull, 0..1.
inline constexpr float kWireTriggerScale = 255.0F;

inline float triggerFraction(std::uint8_t trigger) {
    return static_cast<float>(trigger) / kWireTriggerScale;
}

// The motion wire scale (core/input/UsbReportParsers.h writes it): full scale
// is 2000 deg/s for the gyro and 4 g for the accelerometer at 32767.
inline constexpr float kWireMotionFullScale = 32767.0F;
inline constexpr float kWireGyroFullScaleDps = 2000.0F;
inline constexpr float kWireAccelFullScaleG = 4.0F;

inline float gyroDegreesPerSecond(std::int16_t raw) {
    return static_cast<float>(raw) * kWireGyroFullScaleDps / kWireMotionFullScale;
}

inline float accelG(std::int16_t raw) {
    return static_cast<float>(raw) * kWireAccelFullScaleG / kWireMotionFullScale;
}

// A touch coordinate as a fraction across the pad, 0..1: the wire maps the
// pad's first column or row to -32768 and its last to 32767.
inline constexpr float kWireTouchOffset = 32768.0F;
inline constexpr float kWireTouchSpan = 65535.0F;

inline float touchFraction(std::int16_t coordinate) {
    return (static_cast<float>(coordinate) + kWireTouchOffset) / kWireTouchSpan;
}

} // namespace dish::input
