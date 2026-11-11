// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Battery routing and validation for the MSG_BATTERY (0x000B) telemetry path:
// which (level, status) pair goes on the wire. Two batteries compete, the pad's
// own and this machine's, and the lower one goes; this machine's alone when the
// pad reports none. The live host query and the SDL power-level read stay in
// SDLGamepadBridge::pollBatteries and AppModel's Direct heartbeat.
//
// Wire constants, mirroring satellite/src/core/types.h:
//   level  in [0,100] or 0xFF for unknown
//   status in [0,4]: unknown, discharging, charging, full, wired

#pragma once

#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>

namespace dish::reducer {

// Its own type, rather than util::BatteryReading, so core/reducer stays Qt-free.
struct BatterySample {
    std::uint8_t level = 0;
    std::uint8_t status = 0;

    bool operator==(const BatterySample& o) const { return level == o.level && status == o.status; }
};

inline constexpr std::uint8_t kBatteryLevelUnknown = 0xFF;
inline constexpr std::uint8_t kBatteryStatusUnknown = 0;
inline constexpr std::uint8_t kBatteryStatusDischarging = 1;
inline constexpr std::uint8_t kBatteryStatusCharging = 2;
inline constexpr std::uint8_t kBatteryStatusFull = 3;
inline constexpr std::uint8_t kBatteryStatusWired = 4;
inline constexpr std::uint8_t kBatteryStatusMin = kBatteryStatusUnknown;
inline constexpr std::uint8_t kBatteryStatusMax = kBatteryStatusWired;
// Samples are never coalesced: every one is forwarded, so a lost UDP packet
// self-heals on the next tick. The SDL bridge owns the actual timer.
inline constexpr int kBatteryReportIntervalSeconds = 30;

inline constexpr BatterySample kUnknownBatterySample{kBatteryLevelUnknown, kBatteryStatusUnknown};

// A bogus 101..254 level or an undocumented status is rejected. Validity is
// independent of the previous sample.
inline bool batterySampleValid(const BatterySample& s) {
    const bool levelOk = (s.level <= 100) || (s.level == kBatteryLevelUnknown);
    const bool statusOk = (s.status >= kBatteryStatusMin) && (s.status <= kBatteryStatusMax);
    return levelOk && statusOk;
}

// For callers validating before the narrowing to uint8, so a negative or >255
// value from an upstream decode rejects instead of wrapping into range.
inline bool batterySampleValid(int level, int status) {
    if (level < 0 || level > 0xFF) { return false; }
    if (status < 0 || status > 0xFF) { return false; }
    return batterySampleValid(
        BatterySample{static_cast<std::uint8_t>(level), static_cast<std::uint8_t>(status)});
}

// The platform battery status codes, restated framework-free. They differ from
// the wire status values, so the map below is not the identity.
inline constexpr int kAndroidStatusUnknown = 1;
inline constexpr int kAndroidStatusCharging = 2;
inline constexpr int kAndroidStatusDischarging = 3;
inline constexpr int kAndroidStatusNotCharging = 4;
inline constexpr int kAndroidStatusFull = 5;

// NOT_CHARGING (plugged but held) reports as discharging, to match what the
// player perceives. Anything outside the documented set falls back to unknown.
inline std::uint8_t physicalBatteryStatusToWire(int status) {
    switch (status) {
    case kAndroidStatusCharging:
        return kBatteryStatusCharging;
    case kAndroidStatusFull:
        return kBatteryStatusFull;
    case kAndroidStatusDischarging:
    case kAndroidStatusNotCharging:
        return kBatteryStatusDischarging;
    default:
        return kBatteryStatusUnknown;
    }
}

// `capacity` is a fraction in [0,1]. nullopt means no pad battery at all, so the
// caller falls back to the host; a NaN or negative capacity still yields a sample
// but with the unknown level sentinel.
inline std::optional<BatterySample> physicalBatteryMapping(bool isPresent, float capacity,
                                                           int status) {
    if (!isPresent) { return std::nullopt; }
    std::uint8_t level = kBatteryLevelUnknown;
    if (!std::isnan(capacity) && capacity >= 0.0f) {
        int pct = static_cast<int>(capacity * 100.0f); // truncates, matching the other clients
        if (pct < 0) { pct = 0; }
        if (pct > 100) { pct = 100; }
        level = static_cast<std::uint8_t>(pct);
    }
    return BatterySample{level, physicalBatteryStatusToWire(status)};
}

struct RoutedBattery {
    std::optional<BatterySample> display; // the pad's own reading, for the UI chip
    BatterySample wire;                   // what MSG_BATTERY carries
};

// The unknown sentinel sorts as +infinity, so a known level always wins.
inline int comparableBatteryLevel(const BatterySample& s) {
    return s.level == kBatteryLevelUnknown ? std::numeric_limits<int>::max() : s.level;
}

// On a tie the pad wins, so the result is deterministic.
inline BatterySample lowestBattery(const BatterySample& pad,
                                   const std::optional<BatterySample>& host) {
    if (!host.has_value()) { return pad; }
    return comparableBatteryLevel(*host) < comparableBatteryLevel(pad) ? *host : pad;
}

// The lower of the pad's own reading and this machine's battery goes on the wire,
// however the pad is linked: a pad on a cable that reports its own charge (a
// DualSense charging over USB) competes like any other. This machine's battery
// alone when the pad reports none. The card keeps the pad's own reading.
inline RoutedBattery resolveBattery(const std::optional<BatterySample>& pad,
                                    const std::optional<BatterySample>& host) {
    if (!pad.has_value()) { return RoutedBattery{pad, host.value_or(kUnknownBatterySample)}; }
    return RoutedBattery{pad, lowestBattery(*pad, host)};
}

} // namespace dish::reducer
