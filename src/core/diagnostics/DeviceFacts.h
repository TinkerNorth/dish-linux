// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The PC's own facts that explain a latency or feedback complaint
// (docs/diagnostics-bundle.md, "Device facts"): its battery, whether the
// platform's power profile is the low-power one, and how hot it runs against
// its own trip points. The system layer reads /sys/class/power_supply,
// /sys/firmware/acpi/platform_profile and /sys/class/thermal and passes the
// values in as they came; what they mean is decided here. This client links no
// WLAN library, so `wifi` is not here.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dish::diagnostics {

// One /sys/class/power_supply/<name> entry: its `type`, `scope`, `status` and `capacity` files,
// each empty (or nullopt) where the entry has no such file.
struct PowerSupplyReading {
    std::string type;
    std::string scope;
    std::string status;
    std::optional<int> capacity;
};

struct BatteryFacts {
    std::optional<int> level;
    bool charging = false;
};

struct PowerFacts {
    // Empty on a PC with no system battery.
    std::optional<BatteryFacts> battery;
    // Empty where the platform names no power profile.
    std::optional<bool> powerSave;
};

inline constexpr std::string_view kSupplyTypeBattery = "Battery";
// A peripheral's own battery (a pad's, a mouse's) carries scope "Device"; the system's carries
// "System" or none at all.
inline constexpr std::string_view kSupplyScopeDevice = "Device";
inline constexpr std::string_view kSupplyStatusCharging = "Charging";
// /sys/firmware/acpi/platform_profile's power-saving choice.
inline constexpr std::string_view kLowPowerProfile = "low-power";
inline constexpr int kFullPercent = 100;

inline bool isSystemBattery(const PowerSupplyReading& supply) {
    return supply.type == kSupplyTypeBattery && supply.scope != kSupplyScopeDevice;
}

// The first system battery in `supplies` (the folder's order), and the profile's verdict.
inline PowerFacts powerFactsOf(const std::vector<PowerSupplyReading>& supplies,
                               const std::optional<std::string>& platformProfile) {
    PowerFacts facts;
    if (platformProfile.has_value()) { facts.powerSave = *platformProfile == kLowPowerProfile; }
    for (const PowerSupplyReading& supply : supplies) {
        if (!isSystemBattery(supply)) { continue; }
        BatteryFacts battery;
        const bool levelInRange = supply.capacity.has_value() && *supply.capacity >= 0 &&
                                  *supply.capacity <= kFullPercent;
        if (levelInRange) { battery.level = supply.capacity; }
        battery.charging = supply.status == kSupplyStatusCharging;
        facts.battery = battery;
        return facts;
    }
    return facts;
}

// The contract's words for how hot the device runs.
enum class ThermalState : std::uint8_t { Nominal, Fair, Serious, Critical };

inline std::string_view thermalStateToken(ThermalState state) {
    switch (state) {
    case ThermalState::Nominal:
        return "nominal";
    case ThermalState::Fair:
        return "fair";
    case ThermalState::Serious:
        return "serious";
    case ThermalState::Critical:
        return "critical";
    }
    return "nominal";
}

// One trip point of a zone: its trip_point_N_type and trip_point_N_temp (millidegrees).
struct ThermalTrip {
    std::string type;
    std::int64_t milliCelsius = 0;
};

// One /sys/class/thermal/thermal_zone<N>: its `temp`, empty where it could not be read.
struct ThermalZoneReading {
    std::optional<std::int64_t> milliCelsius;
    std::vector<ThermalTrip> trips;
};

// What crossing each kind of trip means: an `active` trip turns a fan up (fair), a `passive` one
// starts throttling (serious), `hot` and `critical` are the platform's last warnings before it
// shuts down (critical). A trip at or below 0 is a disabled one and a kind not named here says
// nothing.
inline std::optional<ThermalState> stateOfTripType(std::string_view type) {
    if (type == "active") { return ThermalState::Fair; }
    if (type == "passive") { return ThermalState::Serious; }
    if (type == "hot" || type == "critical") { return ThermalState::Critical; }
    return std::nullopt;
}

// The hottest verdict of any zone that has a temperature and a trip point to judge it by; empty
// when no zone has both.
inline std::optional<ThermalState> thermalStateOf(const std::vector<ThermalZoneReading>& zones) {
    std::optional<ThermalState> worst;
    for (const ThermalZoneReading& zone : zones) {
        if (!zone.milliCelsius.has_value()) { continue; }
        const std::int64_t zoneMilliCelsius = *zone.milliCelsius;
        for (const ThermalTrip& trip : zone.trips) {
            const std::optional<ThermalState> meaning = stateOfTripType(trip.type);
            const bool enabled = trip.milliCelsius > 0;
            if (!meaning.has_value() || !enabled) { continue; }
            const bool crossed = zoneMilliCelsius >= trip.milliCelsius;
            const ThermalState state = crossed ? *meaning : ThermalState::Nominal;
            if (!worst.has_value() || state > *worst) { worst = state; }
        }
    }
    return worst;
}

} // namespace dish::diagnostics
