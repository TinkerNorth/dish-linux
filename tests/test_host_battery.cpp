// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

// Coverage for util::hostBatteryFromSysfs — the pure mapping from parsed
// /sys/class/power_supply/* battery entries to the MSG_BATTERY (0x000B) wire
// (level, status) pair. The live readHostBattery() wraps this around a sysfs
// scan; the mapping is split out so every branch is testable without a real
// filesystem. Same pattern as test_satellite_client_motion.cpp — the pure
// function is the seam.

#include "Util/HostBattery.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

using dish::util::BatteryReading;
using dish::util::hostBatteryFromSysfs;
using dish::util::SysfsBattery;

TEST_CASE("no battery devices (a desktop) reports 100% wired", "[hostbattery]") {
    const auto r = hostBatteryFromSysfs({});
    REQUIRE(r.level == 100U);
    REQUIRE(r.status == dish::util::kBatteryStatusWired);
}

TEST_CASE("single discharging battery reports its capacity", "[hostbattery]") {
    const std::vector<SysfsBattery> batteries{{63, "Discharging"}};
    const auto r = hostBatteryFromSysfs(batteries);
    REQUIRE(r.level == 63U);
    REQUIRE(r.status == dish::util::kBatteryStatusDischarging);
}

TEST_CASE("single charging battery maps Charging → charging", "[hostbattery]") {
    const std::vector<SysfsBattery> batteries{{40, "Charging"}};
    const auto r = hostBatteryFromSysfs(batteries);
    REQUIRE(r.level == 40U);
    REQUIRE(r.status == dish::util::kBatteryStatusCharging);
}

TEST_CASE("Full status maps to the full wire status", "[hostbattery]") {
    const std::vector<SysfsBattery> batteries{{100, "Full"}};
    const auto r = hostBatteryFromSysfs(batteries);
    REQUIRE(r.level == 100U);
    REQUIRE(r.status == dish::util::kBatteryStatusFull);
}

TEST_CASE("multiple batteries average their capacities", "[hostbattery]") {
    // A ThinkPad-style dual-battery laptop: 80 % and 40 % → mean 60 %.
    const std::vector<SysfsBattery> batteries{{80, "Discharging"}, {40, "Discharging"}};
    const auto r = hostBatteryFromSysfs(batteries);
    REQUIRE(r.level == 60U);
    REQUIRE(r.status == dish::util::kBatteryStatusDischarging);
}

TEST_CASE("charging on any pack wins over a discharging sibling", "[hostbattery]") {
    // One pack charges while the other drains — the machine is net gaining
    // charge, so CHARGING is the player-meaningful answer.
    const std::vector<SysfsBattery> batteries{{30, "Charging"}, {90, "Discharging"}};
    const auto r = hostBatteryFromSysfs(batteries);
    REQUIRE(r.level == 60U);
    REQUIRE(r.status == dish::util::kBatteryStatusCharging);
}

TEST_CASE("'Not charging' with a topped-out pack reports full", "[hostbattery]") {
    // Some firmware only ever says "Not charging"; a pack sitting at 100 %
    // should still read as full rather than leaving the status unknown.
    const std::vector<SysfsBattery> batteries{{100, "Not charging"}};
    const auto r = hostBatteryFromSysfs(batteries);
    REQUIRE(r.level == 100U);
    REQUIRE(r.status == dish::util::kBatteryStatusFull);
}

TEST_CASE("'Unknown' status on a mid-charge pack maps to unknown status", "[hostbattery]") {
    const std::vector<SysfsBattery> batteries{{55, "Unknown"}};
    const auto r = hostBatteryFromSysfs(batteries);
    REQUIRE(r.level == 55U);
    REQUIRE(r.status == dish::util::kBatteryStatusUnknown);
}

TEST_CASE("capacity is clamped into the 0..100 wire range", "[hostbattery]") {
    // A misbehaving driver can report a capacity above 100; the mapping
    // clamps so the wire byte never exceeds the documented range.
    const std::vector<SysfsBattery> batteries{{135, "Discharging"}};
    const auto r = hostBatteryFromSysfs(batteries);
    REQUIRE(r.level == 100U);
}

TEST_CASE("low battery keeps its level intact (UI styles it, not the wire)", "[hostbattery]") {
    const std::vector<SysfsBattery> batteries{{8, "Discharging"}};
    const auto r = hostBatteryFromSysfs(batteries);
    REQUIRE(r.level == 8U);
    REQUIRE(r.status == dish::util::kBatteryStatusDischarging);
}
