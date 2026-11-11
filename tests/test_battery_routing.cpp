// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The rule: when two batteries compete, the pad's own and this machine's, the
// lower one goes on the wire. There is no transport input: a pad on a cable
// reports its own charge (a DualSense charging over USB reads "charging") and
// competes like any other, so the "wired" cases below are spelled as the
// samples such a pad sends.

#include "core/reducer/BatteryRouting.h"

#include <catch2/catch_test_macros.hpp>

#include <optional>

using dish::reducer::BatterySample;
using dish::reducer::kBatteryLevelUnknown;
using dish::reducer::kBatteryStatusCharging;
using dish::reducer::kBatteryStatusDischarging;
using dish::reducer::kBatteryStatusFull;
using dish::reducer::kBatteryStatusUnknown;
using dish::reducer::kBatteryStatusWired;
using dish::reducer::kUnknownBatterySample;
using dish::reducer::resolveBattery;

namespace {
BatterySample sample(int level, int status) {
    return BatterySample{static_cast<std::uint8_t>(level), static_cast<std::uint8_t>(status)};
}
} // namespace

TEST_CASE("a pad without its own battery displays nothing and sends the host's",
          "[battery][routing]") {
    const auto r = resolveBattery(/*pad=*/std::nullopt,
                                  /*host=*/sample(64, kBatteryStatusDischarging));
    REQUIRE_FALSE(r.display.has_value());
    REQUIRE(r.wire == sample(64, kBatteryStatusDischarging));
}

TEST_CASE("a wired pad lower than the host sends the pad's sample", "[battery][routing]") {
    const auto r = resolveBattery(/*pad=*/sample(20, kBatteryStatusCharging),
                                  /*host=*/sample(80, kBatteryStatusDischarging));
    REQUIRE(r.wire == sample(20, kBatteryStatusCharging));
}

TEST_CASE("a wired pad higher than the host sends the host's sample", "[battery][routing]") {
    const auto r = resolveBattery(/*pad=*/sample(90, kBatteryStatusCharging),
                                  /*host=*/sample(40, kBatteryStatusDischarging));
    REQUIRE(r.wire == sample(40, kBatteryStatusDischarging));
}

TEST_CASE("an unknown host level and a known pad send the pad's sample", "[battery][routing]") {
    // 0xFF sorts as +infinity, so the pad's known level wins.
    const auto r = resolveBattery(/*pad=*/sample(33, kBatteryStatusCharging),
                                  /*host=*/sample(kBatteryLevelUnknown, kBatteryStatusUnknown));
    REQUIRE(r.wire == sample(33, kBatteryStatusCharging));
}

TEST_CASE("a wireless pad lower than the host sends the pad's sample", "[battery][routing]") {
    const auto r = resolveBattery(/*pad=*/sample(30, kBatteryStatusDischarging),
                                  /*host=*/sample(90, kBatteryStatusCharging));
    REQUIRE(r.wire == sample(30, kBatteryStatusDischarging));
}

TEST_CASE("a pad higher than the host still displays its own reading", "[battery][routing]") {
    // Only the wire takes the lower battery; the card shows what the pad says.
    const auto r = resolveBattery(/*pad=*/sample(95, kBatteryStatusFull),
                                  /*host=*/sample(40, kBatteryStatusDischarging));
    REQUIRE(r.display.has_value());
    REQUIRE(*r.display == sample(95, kBatteryStatusFull));
    REQUIRE(r.wire == sample(40, kBatteryStatusDischarging));
}

TEST_CASE("a level tie sends the pad's sample", "[battery][routing]") {
    const auto r = resolveBattery(/*pad=*/sample(50, kBatteryStatusDischarging),
                                  /*host=*/sample(50, kBatteryStatusCharging));
    REQUIRE(r.wire == sample(50, kBatteryStatusDischarging));
}

TEST_CASE("a pad's unknown level loses to a known host level", "[battery][routing]") {
    const auto r = resolveBattery(/*pad=*/sample(kBatteryLevelUnknown, kBatteryStatusUnknown),
                                  /*host=*/sample(33, kBatteryStatusDischarging));
    REQUIRE(r.wire == sample(33, kBatteryStatusDischarging));
}

TEST_CASE("both levels unknown send the pad's sample", "[battery][routing]") {
    const auto r = resolveBattery(/*pad=*/sample(kBatteryLevelUnknown, kBatteryStatusDischarging),
                                  /*host=*/sample(kBatteryLevelUnknown, kBatteryStatusUnknown));
    REQUIRE(r.wire == sample(kBatteryLevelUnknown, kBatteryStatusDischarging));
}

TEST_CASE("an unreadable host leaves the pad's own sample on the wire", "[battery][routing]") {
    const auto r = resolveBattery(/*pad=*/sample(20, kBatteryStatusWired), /*host=*/std::nullopt);
    REQUIRE(r.wire == sample(20, kBatteryStatusWired));
}

TEST_CASE("an unreadable host and no pad battery send the unknown sentinel", "[battery][routing]") {
    const auto r = resolveBattery(/*pad=*/std::nullopt, /*host=*/std::nullopt);
    REQUIRE(r.wire == kUnknownBatterySample);
}

TEST_CASE("the wire sample carries the status of whichever side won", "[battery][routing]") {
    const auto r = resolveBattery(/*pad=*/sample(70, kBatteryStatusDischarging),
                                  /*host=*/sample(25, kBatteryStatusCharging));
    REQUIRE(r.wire.level == 25U);
    REQUIRE(r.wire.status == kBatteryStatusCharging);
}
