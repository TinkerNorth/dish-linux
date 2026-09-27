// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "HostBattery.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>

namespace dish::util {

namespace {

namespace fs = std::filesystem;

// The kernel power-supply class. Each child directory is one supply (a
// battery, an AC adapter, a USB port); the `type` file says which.
constexpr const char* kPowerSupplyRoot = "/sys/class/power_supply";

// At or above this percentage a battery on AC power that is not actively
// charging is treated as FULL — the kernel reports "Not charging" once a pack
// tops out, and a strict "== 100" test would miss the common 99/100 plateau.
constexpr int kFullThresholdPercent = 99;

// Read the whole contents of a sysfs file, trimming the trailing newline.
// Returns an empty string if the file can't be opened.
std::string readSysfsFile(const fs::path& path) {
    std::ifstream in(path);
    if (!in) { return {}; }
    std::string contents;
    std::getline(in, contents);
    // getline already drops the '\n'; strip a stray '\r' / trailing spaces
    // defensively so a string compare against "Battery" still matches.
    while (!contents.empty() &&
           (contents.back() == '\r' || contents.back() == ' ' || contents.back() == '\t')) {
        contents.pop_back();
    }
    return contents;
}

// The integer mean of the readable capacities, so a multi-battery laptop reports one figure, or
// kBatteryLevelUnknown when no pack's capacity could be read. Packs present but all unreadable is
// a different fact from no packs at all, and only the second one means "desktop".
std::uint8_t meanCapacity(const std::vector<SysfsBattery>& batteries) {
    long capacitySum = 0;
    long readable = 0;
    for (const auto& b : batteries) {
        if (!b.capacityKnown) { continue; }
        capacitySum += b.capacity;
        ++readable;
    }
    if (readable == 0) { return kBatteryLevelUnknown; }
    return static_cast<std::uint8_t>(std::clamp(static_cast<int>(capacitySum / readable), 0, 100));
}

// The per-battery `status` texts folded into one wire status. "Charging" anywhere wins (the
// machine is gaining charge); then "Discharging"; then "Full", which a pack at the full plateau
// also counts as. "Not charging" / "Unknown" / anything else contribute nothing.
//
// Never Unknown once a pack exists: firmware that only ever says "Not charging" would otherwise
// put a 0 on the wire where the other clients put a 1, and the satellite reads the two differently.
std::uint8_t foldStatus(const std::vector<SysfsBattery>& batteries, std::uint8_t level) {
    bool anyCharging = false;
    bool anyDischarging = false;
    bool anyFull = false;
    for (const auto& b : batteries) {
        if (b.status == "Charging") {
            anyCharging = true;
        } else if (b.status == "Discharging") {
            anyDischarging = true;
        } else if (b.status == "Full") {
            anyFull = true;
        }
    }
    if (anyCharging) { return kBatteryStatusCharging; }
    const bool atPlateau = level != kBatteryLevelUnknown && level >= kFullThresholdPercent;
    if (!anyDischarging && (anyFull || atPlateau)) { return kBatteryStatusFull; }
    return kBatteryStatusDischarging;
}

} // namespace

BatteryReading hostBatteryFromSysfs(const std::vector<SysfsBattery>& batteries) {
    // No battery devices at all: a desktop. Report a full wired charge, the same value SDL's WIRED
    // power level mapped to before this fallback.
    if (batteries.empty()) { return {100, kBatteryStatusWired}; }
    const std::uint8_t level = meanCapacity(batteries);
    return {level, foldStatus(batteries, level)};
}

BatteryReading readHostBattery() {
    std::vector<SysfsBattery> batteries;
    std::error_code ec;
    fs::directory_iterator it(kPowerSupplyRoot, ec);
    if (!ec) {
        for (const auto& entry : it) {
            const fs::path& dir = entry.path();
            // Only consider supplies whose `type` file reads "Battery" — skip
            // the AC adapter and USB-port entries that share this directory.
            if (readSysfsFile(dir / "type") != "Battery") { continue; }

            SysfsBattery battery;
            const std::string capacityText = readSysfsFile(dir / "capacity");
            try {
                battery.capacity = std::stoi(capacityText);
            } catch (const std::exception&) {
                // Kept in the list with the flag cleared: dropping it here is
                // what made a laptop with one unreadable pack report as a
                // desktop at 100%.
                battery.capacityKnown = false;
            }
            battery.status = readSysfsFile(dir / "status");
            batteries.push_back(std::move(battery));
        }
    }
    return hostBatteryFromSysfs(batteries);
}

} // namespace dish::util
