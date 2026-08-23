// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// When Dish holds the machine awake, and how far. Pure: the inhibitor performs,
// this decides.

#pragma once

#include <cstdint>
#include <string_view>

namespace dish::reducer {

enum class KeepAwakeMode : std::uint8_t {
    Off,
    WhileControllerActive, // streaming, and input seen inside the idle window
    WhileConnected,        // streaming, however long the pad has sat still
};

// How far the hold reaches. Display implies system: a lit screen on a suspended
// machine is not a state any platform offers.
enum class KeepAwakeReach : std::uint8_t { None, System, SystemAndDisplay };

inline constexpr int kKeepAwakeMinTimeoutMinutes = 1;
inline constexpr int kKeepAwakeMaxTimeoutMinutes = 180;
inline constexpr int kKeepAwakeDefaultTimeoutMinutes = 5;

inline constexpr int clampKeepAwakeTimeoutMinutes(int minutes) {
    if (minutes < kKeepAwakeMinTimeoutMinutes) { return kKeepAwakeMinTimeoutMinutes; }
    if (minutes > kKeepAwakeMaxTimeoutMinutes) { return kKeepAwakeMaxTimeoutMinutes; }
    return minutes;
}

struct KeepAwakePreferences {
    KeepAwakeMode mode = KeepAwakeMode::WhileControllerActive;
    int idleTimeoutMinutes = kKeepAwakeDefaultTimeoutMinutes;
    // Off by default: forwarding a pad needs the machine, not the panel.
    bool keepDisplayAwake = false;

    bool operator==(const KeepAwakePreferences& o) const {
        return mode == o.mode && idleTimeoutMinutes == o.idleTimeoutMinutes &&
               keepDisplayAwake == o.keepDisplayAwake;
    }
    bool operator!=(const KeepAwakePreferences& o) const { return !(*this == o); }
};

inline constexpr std::string_view kKeepAwakeModeOff = "off";
inline constexpr std::string_view kKeepAwakeModeWhileControllerActive = "controller-active";
inline constexpr std::string_view kKeepAwakeModeWhileConnected = "connected";

inline constexpr std::string_view keepAwakeModeKey(KeepAwakeMode mode) {
    switch (mode) {
    case KeepAwakeMode::Off:
        return kKeepAwakeModeOff;
    case KeepAwakeMode::WhileConnected:
        return kKeepAwakeModeWhileConnected;
    case KeepAwakeMode::WhileControllerActive:
        break;
    }
    return kKeepAwakeModeWhileControllerActive;
}

// Lenient: an unknown or empty key is the default mode, so a hand-edited or
// downgraded config file cannot leave the machine pinned awake.
inline constexpr KeepAwakeMode keepAwakeModeFromKey(std::string_view key) {
    if (key == kKeepAwakeModeOff) { return KeepAwakeMode::Off; }
    if (key == kKeepAwakeModeWhileConnected) { return KeepAwakeMode::WhileConnected; }
    return KeepAwakeMode::WhileControllerActive;
}

// Whether input at `lastInputMs` is still inside the window at `nowMs`. There is
// no "never seen input" sentinel — 0 is a legitimate stamp on a monotonic clock,
// so the caller owns that fact. A non-positive timeout never expires, and a
// clock that steps backwards reads as activity rather than as an expired window.
inline constexpr bool controllerActiveAt(std::int64_t lastInputMs, std::int64_t nowMs,
                                         std::int64_t timeoutMs) {
    if (timeoutMs <= 0) { return true; }
    if (nowMs <= lastInputMs) { return true; }
    return nowMs - lastInputMs < timeoutMs;
}

// The one rule the inhibitor is driven from.
inline constexpr KeepAwakeReach deriveKeepAwakeReach(const KeepAwakePreferences& prefs,
                                                     int streamingSlotCount,
                                                     bool controllerActive) {
    if (prefs.mode == KeepAwakeMode::Off) { return KeepAwakeReach::None; }
    if (streamingSlotCount <= 0) { return KeepAwakeReach::None; }
    if (prefs.mode == KeepAwakeMode::WhileControllerActive && !controllerActive) {
        return KeepAwakeReach::None;
    }
    return prefs.keepDisplayAwake ? KeepAwakeReach::SystemAndDisplay : KeepAwakeReach::System;
}

} // namespace dish::reducer
