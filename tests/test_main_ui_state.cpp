// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Wire status constants: 0 unknown, 1 discharging, 2 charging, 3 full,
// 4 wired; level 0xFF means unknown.

#include "core/reducer/BatteryUi.h"

#include <catch2/catch_test_macros.hpp>

#include <optional>

namespace br = dish::reducer;

TEST_CASE("MainUiState battery: fromWire keeps a known level and discharging status",
          "[mainui][battery]") {
    const auto b = br::batteryUiFromWire(/*level=*/80, br::kBatteryStatusDischarging);
    REQUIRE(b.has_value());
    REQUIRE(b->level == 80);
    REQUIRE_FALSE(b->charging);
}

TEST_CASE("MainUiState battery: fromWire marks charging/full/wired states as charging",
          "[mainui][battery]") {
    REQUIRE(br::batteryUiFromWire(50, br::kBatteryStatusCharging)->charging);
    REQUIRE(br::batteryUiFromWire(100, br::kBatteryStatusFull)->charging);
    REQUIRE(br::batteryUiFromWire(0xFF, br::kBatteryStatusWired)->charging); // wired desktop pad
}

TEST_CASE("MainUiState battery: fromWire collapses the unknown-level unknown-status pair to null",
          "[mainui][battery]") {
    REQUIRE_FALSE(br::batteryUiFromWire(0xFF, br::kBatteryStatusUnknown).has_value());
}

TEST_CASE("MainUiState battery: fromWire keeps an unknown level when the status is known",
          "[mainui][battery]") {
    // A wired desktop pad reports level 0xFF but a real status, so it still
    // renders — just without a percentage.
    const auto b = br::batteryUiFromWire(0xFF, br::kBatteryStatusWired);
    REQUIRE(b.has_value());
    REQUIRE_FALSE(b->level.has_value());
    REQUIRE(b->charging);
}

TEST_CASE("MainUiState battery: isLow is true only for a low non-charging battery",
          "[mainui][battery]") {
    REQUIRE(br::isLowBattery(br::BatteryUi{15, false}));
    REQUIRE(br::isLowBattery(br::BatteryUi{1, false}));
    // The threshold is INCLUSIVE (<= 15).
    REQUIRE(br::isLowBattery(br::BatteryUi{br::kLowBatteryThreshold, false}));
    REQUIRE_FALSE(br::isLowBattery(br::BatteryUi{16, false}));
    REQUIRE_FALSE(br::isLowBattery(br::BatteryUi{100, false}));
    REQUIRE_FALSE(br::isLowBattery(br::BatteryUi{5, true}));
    REQUIRE_FALSE(br::isLowBattery(br::BatteryUi{std::nullopt, false}));
}

TEST_CASE("MainUiState battery: a low wired pad is not low (wired folds to charging)",
          "[mainui][battery]") {
    const auto b = br::batteryUiFromWire(10, br::kBatteryStatusWired);
    REQUIRE(b.has_value());
    REQUIRE_FALSE(br::isLowBattery(*b));
}
