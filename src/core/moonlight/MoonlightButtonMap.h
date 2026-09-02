// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Maps the repo's internal gamepad report (GamepadInputProcessor's XInput-style
// button word and stick/trigger ranges) onto the Moonlight CONTROLLER_MULTI
// fields. Pure and header-only so the hot path pays no call overhead and the
// mapping is unit-testable without a controller.

#pragma once

#include "core/moonlight/MoonlightProtocol.h"

#include <cstdint>

namespace dish::moonmap {

// The internal button bits (GamepadInputProcessor::Report). Restated here so
// this header stays free of the Input layer.
namespace inbtn {
inline constexpr std::uint16_t kDpadUp = 0x0001;
inline constexpr std::uint16_t kDpadDown = 0x0002;
inline constexpr std::uint16_t kDpadLeft = 0x0004;
inline constexpr std::uint16_t kDpadRight = 0x0008;
inline constexpr std::uint16_t kStart = 0x0010;
inline constexpr std::uint16_t kBack = 0x0020;
inline constexpr std::uint16_t kLeftThumb = 0x0040;
inline constexpr std::uint16_t kRightThumb = 0x0080;
inline constexpr std::uint16_t kLeftShoulder = 0x0100;
inline constexpr std::uint16_t kRightShoulder = 0x0200;
inline constexpr std::uint16_t kA = 0x1000;
inline constexpr std::uint16_t kB = 0x2000;
inline constexpr std::uint16_t kX = 0x4000;
inline constexpr std::uint16_t kY = 0x8000;
} // namespace inbtn

// Translates the internal button word into Moonlight's effective button flags.
// The two vocabularies mostly line up (both descend from XInput) but Home/Guide
// and the stick clicks sit at different bits, so the map is explicit.
//
// The explicitness is load-bearing the other way too: 0x0800 has no XINPUT
// assignment and protocol 2 spends it on the DualSense mic-mute STATE
// (input::layout::kXusbMicMute), a Satellite-only signal a GameStream host
// would misread. A Direct-claimed DualSense's decoder folds that bit into
// every report while muted; this map has no row for it, so it can never leak
// to a Moonlight host — a pin test holds that door shut.
inline std::uint32_t toMoonlightButtons(std::uint16_t buttons) {
    std::uint32_t out = 0;
    const auto set = [&](std::uint16_t in, std::uint32_t flag) {
        if ((buttons & in) != 0) { out |= flag; }
    };
    set(inbtn::kDpadUp, moonproto::kBtnDpadUp);
    set(inbtn::kDpadDown, moonproto::kBtnDpadDown);
    set(inbtn::kDpadLeft, moonproto::kBtnDpadLeft);
    set(inbtn::kDpadRight, moonproto::kBtnDpadRight);
    set(inbtn::kStart, moonproto::kBtnStart);
    set(inbtn::kBack, moonproto::kBtnBack);
    set(inbtn::kLeftThumb, moonproto::kBtnLeftStick);
    set(inbtn::kRightThumb, moonproto::kBtnRightStick);
    set(inbtn::kLeftShoulder, moonproto::kBtnLeftButton);
    set(inbtn::kRightShoulder, moonproto::kBtnRightButton);
    set(inbtn::kA, moonproto::kBtnA);
    set(inbtn::kB, moonproto::kBtnB);
    set(inbtn::kX, moonproto::kBtnX);
    set(inbtn::kY, moonproto::kBtnY);
    return out;
}

} // namespace dish::moonmap
