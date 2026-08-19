// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "core/reducer/BatteryRouting.h"

#include <catch2/catch_test_macros.hpp>

#include <optional>

using dish::reducer::BatterySample;
using dish::reducer::kBatteryLevelUnknown;
using dish::reducer::kBatteryStatusCharging;
using dish::reducer::kBatteryStatusDischarging;
using dish::reducer::kBatteryStatusUnknown;
using dish::reducer::kBatteryStatusWired;
using dish::reducer::kUnknownBatterySample;
using dish::reducer::resolveBattery;

namespace {
BatterySample sample(int level, int status) {
    return BatterySample{static_cast<std::uint8_t>(level), static_cast<std::uint8_t>(status)};
}
} // namespace

TEST_CASE("wired pad without its own battery displays nothing and wires the host",
          "[battery][routing]") {
    const auto r = resolveBattery(/*padWired=*/true, /*pad=*/std::nullopt,
                                  /*host=*/sample(64, kBatteryStatusDischarging));
    REQUIRE_FALSE(r.display.has_value());
    REQUIRE(r.wire == sample(64, kBatteryStatusDischarging));
}

TEST_CASE("wireless pad without its own battery displays nothing and wires the host",
          "[battery][routing]") {
    const auto r = resolveBattery(/*padWired=*/false, /*pad=*/std::nullopt,
                                  /*host=*/sample(64, kBatteryStatusDischarging));
    REQUIRE_FALSE(r.display.has_value());
    REQUIRE(r.wire == sample(64, kBatteryStatusDischarging));
}

TEST_CASE("wired pad with a battery displays the device but wires the host", "[battery][routing]") {
    // A wired pad has no meaningful own charge, so the host charge rides the wire.
    const auto r = resolveBattery(/*padWired=*/true, /*pad=*/sample(20, kBatteryStatusWired),
                                  /*host=*/sample(80, kBatteryStatusCharging));
    REQUIRE(r.display.has_value());
    REQUIRE(*r.display == sample(20, kBatteryStatusWired));
    REQUIRE(r.wire == sample(80, kBatteryStatusCharging));
}

TEST_CASE("wireless pad lower than the host displays and wires the device", "[battery][routing]") {
    const auto r = resolveBattery(/*padWired=*/false, /*pad=*/sample(30, kBatteryStatusDischarging),
                                  /*host=*/sample(90, kBatteryStatusCharging));
    REQUIRE(*r.display == sample(30, kBatteryStatusDischarging));
    REQUIRE(r.wire == sample(30, kBatteryStatusDischarging));
}

TEST_CASE("wireless pad higher than the host displays the device but wires the host",
          "[battery][routing]") {
    const auto r = resolveBattery(/*padWired=*/false, /*pad=*/sample(95, kBatteryStatusDischarging),
                                  /*host=*/sample(40, kBatteryStatusDischarging));
    REQUIRE(*r.display == sample(95, kBatteryStatusDischarging));
    REQUIRE(r.wire == sample(40, kBatteryStatusDischarging));
}

TEST_CASE("wireless level tie wires the device sample", "[battery][routing]") {
    const auto r = resolveBattery(/*padWired=*/false, /*pad=*/sample(50, kBatteryStatusDischarging),
                                  /*host=*/sample(50, kBatteryStatusCharging));
    REQUIRE(r.wire == sample(50, kBatteryStatusDischarging));
}

TEST_CASE("wireless pad with unknown level loses the lowest pick to a known host level",
          "[battery][routing]") {
    // 0xFF is treated as +infinity, so the known host level wins.
    const auto r = resolveBattery(/*padWired=*/false,
                                  /*pad=*/sample(kBatteryLevelUnknown, kBatteryStatusUnknown),
                                  /*host=*/sample(33, kBatteryStatusDischarging));
    REQUIRE(r.wire == sample(33, kBatteryStatusDischarging));
}

TEST_CASE("host with unknown level loses the lowest pick to a known device level",
          "[battery][routing]") {
    const auto r = resolveBattery(/*padWired=*/false, /*pad=*/sample(33, kBatteryStatusDischarging),
                                  /*host=*/sample(kBatteryLevelUnknown, kBatteryStatusUnknown));
    REQUIRE(r.wire == sample(33, kBatteryStatusDischarging));
}

TEST_CASE("both levels unknown wires the device sample", "[battery][routing]") {
    const auto r = resolveBattery(/*padWired=*/false,
                                  /*pad=*/sample(kBatteryLevelUnknown, kBatteryStatusDischarging),
                                  /*host=*/sample(kBatteryLevelUnknown, kBatteryStatusUnknown));
    REQUIRE(r.wire == sample(kBatteryLevelUnknown, kBatteryStatusDischarging));
}

TEST_CASE("unreadable host battery on a wired pad wires the unknown sentinel",
          "[battery][routing]") {
    const auto r = resolveBattery(/*padWired=*/true, /*pad=*/sample(20, kBatteryStatusWired),
                                  /*host=*/std::nullopt);
    REQUIRE(r.wire == kUnknownBatterySample);
}

TEST_CASE("unreadable host battery without a device battery wires the unknown sentinel",
          "[battery][routing]") {
    const auto r = resolveBattery(/*padWired=*/false, /*pad=*/std::nullopt, /*host=*/std::nullopt);
    REQUIRE(r.wire == kUnknownBatterySample);
}

TEST_CASE("wire sample carries the status of whichever side won the lowest pick",
          "[battery][routing]") {
    const auto r = resolveBattery(/*padWired=*/false, /*pad=*/sample(70, kBatteryStatusDischarging),
                                  /*host=*/sample(25, kBatteryStatusCharging));
    REQUIRE(r.wire.level == 25U);
    REQUIRE(r.wire.status == kBatteryStatusCharging);
}
