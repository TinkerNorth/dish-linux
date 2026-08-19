// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The single battery-display projection, so the widget and QML paths cannot
// diverge on the low-battery rule. Emits render tokens, never user strings, so
// localization stays in the view layer. Wire constants come from
// BatteryRouting.h: level 0xFF is unknown; status 0 unknown / 1 discharging /
// 2 charging / 3 full / 4 wired.

#pragma once

#include "core/reducer/BatteryRouting.h"

#include <optional>

namespace dish::reducer {

// Inclusive: a 15% pad is low. Shared with the other Dish clients.
inline constexpr int kLowBatteryThreshold = 15;

// `charging` folds the charging/full/wired wire statuses, because a wired pad has
// nothing to drain and so can never read as low.
struct BatteryUi {
    std::optional<int> level;
    bool charging = false;

    bool operator==(const BatteryUi& o) const { return level == o.level && charging == o.charging; }
    bool operator!=(const BatteryUi& o) const { return !(*this == o); }
};

// An unknown level is not low, because it cannot be judged.
inline bool isLowBattery(const BatteryUi& b) {
    return b.level.has_value() && !b.charging && *b.level <= kLowBatteryThreshold;
}

// Empty only when both level and status are unknown. A wired pad still renders,
// as charging, just without a number.
inline std::optional<BatteryUi> batteryUiFromWire(int level, int status) {
    const bool charging = status == kBatteryStatusCharging || status == kBatteryStatusFull ||
                          status == kBatteryStatusWired;
    const std::optional<int> pct =
        (level == kBatteryLevelUnknown) ? std::nullopt : std::optional<int>(level);
    if (!pct.has_value() && status == kBatteryStatusUnknown) { return std::nullopt; }
    return BatteryUi{pct, charging};
}

// ── Chip projection ──────────────────────────────────────────────────────────
enum class BatteryChipKind {
    None,     // level unknown; no chip at all
    Wired,    // level not rendered
    Charging, // rendered with the level
    Full,     // level not rendered
    Level,    // plain percentage, with `low` driving the warning style
};

struct BatteryChip {
    BatteryChipKind kind = BatteryChipKind::None;
    int level = 0;    // meaningful for Charging and Level
    bool low = false; // Level arm only: charging/full/wired can never be low

    bool operator==(const BatteryChip& o) const {
        return kind == o.kind && level == o.level && low == o.low;
    }
    bool operator!=(const BatteryChip& o) const { return !(*this == o); }
};

// An unknown level hides the chip regardless of status: no reading has landed,
// and a bare status with no percentage is not worth a pill.
inline BatteryChip batteryChip(int level, int status) {
    if (level == kBatteryLevelUnknown) { return BatteryChip{}; }
    BatteryChip chip;
    chip.level = level;
    if (status == kBatteryStatusWired) {
        chip.kind = BatteryChipKind::Wired;
    } else if (status == kBatteryStatusCharging) {
        chip.kind = BatteryChipKind::Charging;
    } else if (status == kBatteryStatusFull) {
        chip.kind = BatteryChipKind::Full;
    } else {
        chip.kind = BatteryChipKind::Level;
        chip.low = isLowBattery(BatteryUi{level, /*charging=*/false});
    }
    return chip;
}

} // namespace dish::reducer
