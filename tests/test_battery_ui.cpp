// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "core/reducer/BatteryUi.h"

#include <catch2/catch_test_macros.hpp>

#include <optional>

namespace br = dish::reducer;

namespace {

// In wire order, per BatteryRouting.h.
const int kAllStatuses[] = {
    br::kBatteryStatusUnknown, br::kBatteryStatusDischarging, br::kBatteryStatusCharging,
    br::kBatteryStatusFull,    br::kBatteryStatusWired,
};

bool foldsToCharging(int status) {
    return status == br::kBatteryStatusCharging || status == br::kBatteryStatusFull ||
           status == br::kBatteryStatusWired;
}

} // namespace

TEST_CASE("BatteryUi fromWire matrix: every status with a known level renders it", "[battery-ui]") {
    for (const int status : kAllStatuses) {
        CAPTURE(status);
        const auto b = br::batteryUiFromWire(/*level=*/50, status);
        REQUIRE(b.has_value());
        CHECK(b->level == 50);
        CHECK(b->charging == foldsToCharging(status));
    }
}

TEST_CASE("BatteryUi fromWire matrix: the unknown-level sentinel keeps only known statuses",
          "[battery-ui]") {
    CHECK_FALSE(br::batteryUiFromWire(0xFF, br::kBatteryStatusUnknown).has_value());
    for (const int status : kAllStatuses) {
        if (status == br::kBatteryStatusUnknown) { continue; }
        CAPTURE(status);
        const auto b = br::batteryUiFromWire(0xFF, status);
        REQUIRE(b.has_value());
        CHECK_FALSE(b->level.has_value());
        CHECK(b->charging == foldsToCharging(status));
    }
}

TEST_CASE("BatteryUi isLow threshold is 15 and inclusive", "[battery-ui]") {
    CHECK(br::kLowBatteryThreshold == 15);
    CHECK(br::isLowBattery(br::BatteryUi{0, false}));
    CHECK(br::isLowBattery(br::BatteryUi{1, false}));
    CHECK(br::isLowBattery(br::BatteryUi{14, false}));
    CHECK(br::isLowBattery(br::BatteryUi{15, false}));
    CHECK_FALSE(br::isLowBattery(br::BatteryUi{16, false}));
    CHECK_FALSE(br::isLowBattery(br::BatteryUi{100, false}));
    CHECK_FALSE(br::isLowBattery(br::BatteryUi{15, true}));
    CHECK_FALSE(br::isLowBattery(br::BatteryUi{1, true}));
    // An unknown percentage cannot be judged, so it is never low.
    CHECK_FALSE(br::isLowBattery(br::BatteryUi{std::nullopt, false}));
    CHECK_FALSE(br::isLowBattery(br::BatteryUi{std::nullopt, true}));
}

TEST_CASE("batteryChip hides the chip for an unknown level regardless of status", "[battery-ui]") {
    for (const int status : kAllStatuses) {
        CAPTURE(status);
        const auto chip = br::batteryChip(0xFF, status);
        CHECK(chip.kind == br::BatteryChipKind::None);
        CHECK(chip == br::BatteryChip{});
    }
}

TEST_CASE("batteryChip maps the wired status to the Wired kind and never low", "[battery-ui]") {
    const auto chip = br::batteryChip(10, br::kBatteryStatusWired);
    CHECK(chip.kind == br::BatteryChipKind::Wired);
    // wired folds to charging, and charging is never low.
    CHECK_FALSE(chip.low);
}

TEST_CASE("batteryChip maps the charging status to the Charging kind with the level",
          "[battery-ui]") {
    const auto chip = br::batteryChip(42, br::kBatteryStatusCharging);
    CHECK(chip.kind == br::BatteryChipKind::Charging);
    CHECK(chip.level == 42);
    CHECK_FALSE(chip.low);
    CHECK_FALSE(br::batteryChip(5, br::kBatteryStatusCharging).low);
}

TEST_CASE("batteryChip maps the full status to the Full kind and never low", "[battery-ui]") {
    const auto chip = br::batteryChip(100, br::kBatteryStatusFull);
    CHECK(chip.kind == br::BatteryChipKind::Full);
    CHECK_FALSE(chip.low);
    CHECK_FALSE(br::batteryChip(10, br::kBatteryStatusFull).low);
}

TEST_CASE("batteryChip renders a plain level for discharging and unknown statuses",
          "[battery-ui]") {
    for (const int status : {br::kBatteryStatusDischarging, br::kBatteryStatusUnknown}) {
        CAPTURE(status);
        const auto chip = br::batteryChip(80, status);
        CHECK(chip.kind == br::BatteryChipKind::Level);
        CHECK(chip.level == 80);
        CHECK_FALSE(chip.low);
    }
}

TEST_CASE("batteryChip low flag is inclusive at 15 and fires only in the level arm",
          "[battery-ui]") {
    CHECK(br::batteryChip(15, br::kBatteryStatusDischarging).low);
    CHECK(br::batteryChip(15, br::kBatteryStatusUnknown).low);
    CHECK(br::batteryChip(1, br::kBatteryStatusDischarging).low);
    CHECK_FALSE(br::batteryChip(16, br::kBatteryStatusDischarging).low);
    CHECK_FALSE(br::batteryChip(15, br::kBatteryStatusCharging).low);
    CHECK_FALSE(br::batteryChip(15, br::kBatteryStatusFull).low);
    CHECK_FALSE(br::batteryChip(15, br::kBatteryStatusWired).low);
}

TEST_CASE("batteryChip full level-by-status matrix", "[battery-ui]") {
    const int levels[] = {0, 1, 14, 15, 16, 50, 100};
    for (const int level : levels) {
        for (const int status : kAllStatuses) {
            CAPTURE(level, status);
            const auto chip = br::batteryChip(level, status);
            if (status == br::kBatteryStatusWired) {
                CHECK(chip.kind == br::BatteryChipKind::Wired);
            } else if (status == br::kBatteryStatusCharging) {
                CHECK(chip.kind == br::BatteryChipKind::Charging);
            } else if (status == br::kBatteryStatusFull) {
                CHECK(chip.kind == br::BatteryChipKind::Full);
            } else {
                CHECK(chip.kind == br::BatteryChipKind::Level);
            }
            CHECK(chip.level == level);
            const bool levelArm =
                status == br::kBatteryStatusUnknown || status == br::kBatteryStatusDischarging;
            CHECK(chip.low == (levelArm && level <= br::kLowBatteryThreshold));
        }
    }
}
