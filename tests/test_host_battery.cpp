// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

// util::hostBatteryFromSysfs — the pure mapping from parsed
// /sys/class/power_supply/* entries to the MSG_BATTERY (level, status) pair.
// readHostBattery() wraps this around a live scan; the mapping is split out so
// every branch is testable without a real filesystem.

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

TEST_CASE("a pack with no readable capacity is unknown, not a desktop", "[hostbattery]") {
    // "No batteries found" and "batteries found but unreadable" are different
    // facts, and only the first one means a desktop at 100 %.
    SECTION("the only pack") {
        const std::vector<SysfsBattery> batteries{{0, "Discharging", false}};
        const auto r = hostBatteryFromSysfs(batteries);
        REQUIRE(r.level == dish::util::kBatteryLevelUnknown);
        REQUIRE(r.status == dish::util::kBatteryStatusDischarging);
    }
    SECTION("a readable sibling still carries the level") {
        const std::vector<SysfsBattery> batteries{{0, "Discharging", false}, {40, "Discharging"}};
        const auto r = hostBatteryFromSysfs(batteries);
        REQUIRE(r.level == 40U);
    }
    SECTION("an unreadable pack cannot reach the full plateau") {
        const std::vector<SysfsBattery> batteries{{0, "Not charging", false}};
        const auto r = hostBatteryFromSysfs(batteries);
        REQUIRE(r.level == dish::util::kBatteryLevelUnknown);
        REQUIRE(r.status == dish::util::kBatteryStatusDischarging);
    }
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

TEST_CASE("'Unknown' status on a mid-charge pack falls back to discharging", "[hostbattery]") {
    // Once a pack exists the status is never Unknown: a 0 on the wire is read
    // differently by the satellite than the 1 the other clients send.
    const std::vector<SysfsBattery> batteries{{55, "Unknown"}};
    const auto r = hostBatteryFromSysfs(batteries);
    REQUIRE(r.level == 55U);
    REQUIRE(r.status == dish::util::kBatteryStatusDischarging);
}

TEST_CASE("the full plateau starts at 99, not at 100", "[hostbattery]") {
    // The kernel stops saying "Charging" a point or two shy of 100, so the
    // threshold is 99; 98 is the last level that still reads as discharging.
    SECTION("98 is not yet full") {
        const std::vector<SysfsBattery> batteries{{98, "Not charging"}};
        const auto r = hostBatteryFromSysfs(batteries);
        REQUIRE(r.level == 98U);
        REQUIRE(r.status == dish::util::kBatteryStatusDischarging);
    }
    SECTION("99 is full") {
        const std::vector<SysfsBattery> batteries{{99, "Not charging"}};
        const auto r = hostBatteryFromSysfs(batteries);
        REQUIRE(r.level == 99U);
        REQUIRE(r.status == dish::util::kBatteryStatusFull);
    }
}

TEST_CASE("a discharging sibling keeps a topped-out pack from reading full", "[hostbattery]") {
    // The machine is losing charge overall, so "full" would be a lie even
    // though one pack says Full and the mean clears the threshold.
    const std::vector<SysfsBattery> batteries{{100, "Full"}, {98, "Discharging"}};
    const auto r = hostBatteryFromSysfs(batteries);
    REQUIRE(r.level == 99U);
    REQUIRE(r.status == dish::util::kBatteryStatusDischarging);
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

TEST_CASE("an empty pack reports 0, not the unknown sentinel", "[hostbattery]") {
    const std::vector<SysfsBattery> batteries{{0, "Discharging"}};
    const auto r = hostBatteryFromSysfs(batteries);
    REQUIRE(r.level == 0U);
    REQUIRE(r.level != dish::util::kBatteryLevelUnknown);
    REQUIRE(r.status == dish::util::kBatteryStatusDischarging);
}

TEST_CASE("a negative capacity clamps to 0 rather than wrapping", "[hostbattery]") {
    // The level is a uint8 on the wire, so an unclamped -1 would go out as 255
    // — the unknown sentinel — and read as "no reading" instead of "flat".
    const std::vector<SysfsBattery> batteries{{-1, "Discharging"}};
    const auto r = hostBatteryFromSysfs(batteries);
    REQUIRE(r.level == 0U);
    REQUIRE(r.status == dish::util::kBatteryStatusDischarging);
}
