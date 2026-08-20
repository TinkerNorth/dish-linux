// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// batterySampleValid: level in [0,100] u {0xFF}, status in [0,4].

#include "core/reducer/BatteryRouting.h"

#include <catch2/catch_test_macros.hpp>

using dish::reducer::BatterySample;
using dish::reducer::batterySampleValid;
using dish::reducer::kBatteryLevelUnknown;
using dish::reducer::kBatteryReportIntervalSeconds;
using dish::reducer::kBatteryStatusCharging;
using dish::reducer::kBatteryStatusDischarging;
using dish::reducer::kBatteryStatusFull;
using dish::reducer::kBatteryStatusUnknown;
using dish::reducer::kBatteryStatusWired;

TEST_CASE("battery validator accepts an ordinary in-range sample", "[battery][validator]") {
    REQUIRE(batterySampleValid(BatterySample{75, kBatteryStatusDischarging}));
}

TEST_CASE("battery validator accepts the level endpoints 0 and 100", "[battery][validator]") {
    REQUIRE(batterySampleValid(BatterySample{0, kBatteryStatusDischarging}));
    REQUIRE(batterySampleValid(BatterySample{100, kBatteryStatusFull}));
}

TEST_CASE("battery validator accepts 0xFF as the unknown level sentinel", "[battery][validator]") {
    REQUIRE(batterySampleValid(BatterySample{kBatteryLevelUnknown, kBatteryStatusUnknown}));
}

TEST_CASE("battery validator rejects a bogus level above 100 (but not 0xFF)",
          "[battery][validator]") {
    REQUIRE_FALSE(batterySampleValid(BatterySample{101, kBatteryStatusDischarging}));
    REQUIRE_FALSE(batterySampleValid(BatterySample{200, kBatteryStatusDischarging}));
    REQUIRE_FALSE(batterySampleValid(BatterySample{254, kBatteryStatusDischarging}));
    // 0xFF is the unknown-level sentinel, not an out-of-range level.
    REQUIRE(batterySampleValid(BatterySample{0xFF, kBatteryStatusDischarging}));
}

TEST_CASE("battery validator rejects a negative level via the int overload",
          "[battery][validator]") {
    REQUIRE_FALSE(batterySampleValid(-1, kBatteryStatusDischarging));
    REQUIRE_FALSE(batterySampleValid(-100, kBatteryStatusDischarging));
}

TEST_CASE("battery validator accepts every documented status code", "[battery][validator]") {
    for (int s = kBatteryStatusUnknown; s <= kBatteryStatusWired; ++s) {
        REQUIRE(batterySampleValid(50, s));
    }
}

TEST_CASE("battery validator rejects a status outside the documented set", "[battery][validator]") {
    REQUIRE_FALSE(batterySampleValid(50, 5));
    REQUIRE_FALSE(batterySampleValid(50, 99));
    REQUIRE_FALSE(batterySampleValid(50, -1));
}

TEST_CASE("battery validator wire constants match the protocol spec", "[battery][validator]") {
    // Pinned against satellite/src/core/types.h so a drift fails here.
    REQUIRE(kBatteryLevelUnknown == 0xFF);
    REQUIRE(kBatteryStatusUnknown == 0);
    REQUIRE(kBatteryStatusDischarging == 1);
    REQUIRE(kBatteryStatusCharging == 2);
    REQUIRE(kBatteryStatusFull == 3);
    REQUIRE(kBatteryStatusWired == 4);
    REQUIRE(kBatteryReportIntervalSeconds == 30);
}
