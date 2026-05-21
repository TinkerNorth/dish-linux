// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dish::util {

// Host-machine battery fallback for the MSG_BATTERY (0x000B) stream.
//
// SDLGamepadBridge reports the *controller's* own battery whenever the pad
// exposes a usable percentage (a wireless DualSense / Switch Pro at LOW /
// MEDIUM / FULL). When the pad is wired (USB) or SDL can't read a level, the
// controller's battery is meaningless — the player wants to know the *host*
// machine's charge instead. readHostBattery() supplies that fallback: on a
// laptop it returns the system battery percentage + charging state; on a
// desktop (no internal battery) it returns 100 % / WIRED so the satellite
// shows a full charge.
//
// The (level, status) pair is the same shape SDLGamepadBridge's
// powerLevelToWire produces and feeds straight into MSG_BATTERY. `level` is
// 0..100 percent or kBatteryLevelUnknown (0xFF); `status` is one of the
// kBatteryStatus* constants — the satellite/src/core/types.h mirrors.
struct BatteryReading {
    std::uint8_t level;
    std::uint8_t status;
};

inline constexpr std::uint8_t kBatteryLevelUnknown = 0xFF;
inline constexpr std::uint8_t kBatteryStatusUnknown = 0;
inline constexpr std::uint8_t kBatteryStatusDischarging = 1;
inline constexpr std::uint8_t kBatteryStatusCharging = 2;
inline constexpr std::uint8_t kBatteryStatusFull = 3;
inline constexpr std::uint8_t kBatteryStatusWired = 4;

// One battery's worth of /sys/class/power_supply/<dev>/ data, lifted into a
// plain struct so the (raw inputs → wire) logic is unit-testable without a
// live sysfs scan. Mirrors the sysfs files verbatim:
//   * capacity — integer percent, 0..100.
//   * status   — the `status` file's text: "Charging", "Discharging",
//                "Full", "Not charging", or "Unknown".
struct SysfsBattery {
    int capacity = 0;
    std::string status;
};

// Pure mapping: a list of detected batteries → BatteryReading. No filesystem
// dependency, so unit tests can pin every branch. Rules:
//   * empty list (no battery devices, i.e. a desktop) → 100 / WIRED.
//   * level is the integer mean of the per-battery capacities.
//   * status is folded across the batteries: "Charging" anywhere wins, then
//     "Discharging", then "Full"; "Not charging" / "Unknown" map to UNKNOWN.
//     Mixed states resolve to the most "active" one so a player charging one
//     of two packs still sees CHARGING.
BatteryReading hostBatteryFromSysfs(const std::vector<SysfsBattery>& batteries);

// Scan /sys/class/power_supply/* for entries whose `type` file reads
// "Battery", read each one's `capacity` + `status`, and run the result
// through hostBatteryFromSysfs. On a desktop the scan finds nothing and the
// reading is {100, WIRED}; on a laptop it is the live percentage + charging
// state, averaged if the machine has more than one battery.
BatteryReading readHostBattery();

} // namespace dish::util
