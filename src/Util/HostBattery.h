// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dish::util {

// Host-machine battery fallback for MSG_BATTERY, used when the pad is wired or
// SDL cannot read its level (the controller's own reading is meaningless then).
//
// Wire shape: `level` is 0..100 percent or kBatteryLevelUnknown (0xFF),
// `status` one of the kBatteryStatus* constants. Both must match
// satellite/src/core/types.h.
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

// One /sys/class/power_supply/<dev>/ entry lifted into a plain struct so the
// mapping is testable without a live sysfs scan. `status` is the file's text:
// "Charging", "Discharging", "Full", "Not charging", or "Unknown".
struct SysfsBattery {
    int capacity = 0;
    std::string status;
    // Last so the two-field brace form still reads as {capacity, status}.
    // False for a pack whose `capacity` file is missing or unparsable: the
    // pack still counts as present, so an unreadable one reads as an unknown
    // level rather than collapsing the machine to "desktop, fully charged".
    bool capacityKnown = true;
};

// An empty list is a desktop and reads 100 / WIRED. Level is the integer mean
// of the capacities; status folds to the most active state across packs
// (Charging > Discharging > Full), so charging one of two packs reads CHARGING.
BatteryReading hostBatteryFromSysfs(const std::vector<SysfsBattery>& batteries);

// hostBatteryFromSysfs over a live scan of /sys/class/power_supply/*.
BatteryReading readHostBattery();

} // namespace dish::util
