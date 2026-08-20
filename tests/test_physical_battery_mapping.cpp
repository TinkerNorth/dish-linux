// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// kAndroidStatus* are the platform status codes the shared reducer accepts; they
// are not the wire values, so the map under test is not the identity.

#include "core/reducer/BatteryRouting.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>

using dish::reducer::batterySampleValid;
using dish::reducer::kAndroidStatusCharging;
using dish::reducer::kAndroidStatusDischarging;
using dish::reducer::kAndroidStatusFull;
using dish::reducer::kAndroidStatusNotCharging;
using dish::reducer::kAndroidStatusUnknown;
using dish::reducer::kBatteryLevelUnknown;
using dish::reducer::kBatteryStatusCharging;
using dish::reducer::kBatteryStatusDischarging;
using dish::reducer::kBatteryStatusFull;
using dish::reducer::kBatteryStatusUnknown;
using dish::reducer::physicalBatteryMapping;
using dish::reducer::physicalBatteryStatusToWire;

TEST_CASE("physicalBatteryMapping: pad with no battery present maps to nullopt",
          "[battery][mapping]") {
    // nullopt tells the caller to fall back to the host battery.
    REQUIRE_FALSE(
        physicalBatteryMapping(/*isPresent=*/false, 0.5f, kAndroidStatusDischarging).has_value());
}

TEST_CASE("physicalBatteryMapping: not-present wins even when a capacity is set",
          "[battery][mapping]") {
    REQUIRE_FALSE(
        physicalBatteryMapping(/*isPresent=*/false, 0.99f, kAndroidStatusFull).has_value());
}

TEST_CASE("physicalBatteryMapping: capacity maps to a percentage in 0..100", "[battery][mapping]") {
    const auto s = physicalBatteryMapping(true, 0.84f, kAndroidStatusDischarging);
    REQUIRE(s.has_value());
    REQUIRE(s->level == 84U);
}

TEST_CASE("physicalBatteryMapping: capacity endpoints map to 0 and 100", "[battery][mapping]") {
    const auto lo = physicalBatteryMapping(true, 0.0f, kAndroidStatusDischarging);
    REQUIRE(lo.has_value());
    REQUIRE(lo->level == 0U);
    const auto hi = physicalBatteryMapping(true, 1.0f, kAndroidStatusFull);
    REQUIRE(hi.has_value());
    REQUIRE(hi->level == 100U);
}

TEST_CASE("physicalBatteryMapping: capacity above 1 is clamped to 100", "[battery][mapping]") {
    const auto s = physicalBatteryMapping(true, 1.02f, kAndroidStatusFull);
    REQUIRE(s.has_value());
    REQUIRE(s->level == 100U);
}

TEST_CASE("physicalBatteryMapping: NaN capacity reports the unknown-level sentinel",
          "[battery][mapping]") {
    const auto s = physicalBatteryMapping(true, std::nanf(""), kAndroidStatusDischarging);
    REQUIRE(s.has_value());
    REQUIRE(s->level == kBatteryLevelUnknown);
}

TEST_CASE("physicalBatteryMapping: negative capacity reports the unknown-level sentinel",
          "[battery][mapping]") {
    const auto s = physicalBatteryMapping(true, -1.0f, kAndroidStatusDischarging);
    REQUIRE(s.has_value());
    REQUIRE(s->level == kBatteryLevelUnknown);
}

TEST_CASE("physicalBatteryMapping: capacity truncates toward zero (not rounds)",
          "[battery][mapping]") {
    // 0.849 * 100 = 84.9 -> 84.
    const auto s = physicalBatteryMapping(true, 0.849f, kAndroidStatusDischarging);
    REQUIRE(s.has_value());
    REQUIRE(s->level == 84U);
}

TEST_CASE("physicalBatteryStatusToWire: charging maps to wire charging", "[battery][mapping]") {
    REQUIRE(physicalBatteryStatusToWire(kAndroidStatusCharging) == kBatteryStatusCharging);
}

TEST_CASE("physicalBatteryStatusToWire: full maps to wire full", "[battery][mapping]") {
    REQUIRE(physicalBatteryStatusToWire(kAndroidStatusFull) == kBatteryStatusFull);
}

TEST_CASE("physicalBatteryStatusToWire: discharging maps to wire discharging",
          "[battery][mapping]") {
    REQUIRE(physicalBatteryStatusToWire(kAndroidStatusDischarging) == kBatteryStatusDischarging);
}

TEST_CASE("physicalBatteryStatusToWire: not-charging reads as discharging", "[battery][mapping]") {
    // Plugged-but-held is user-perceived as discharging.
    REQUIRE(physicalBatteryStatusToWire(kAndroidStatusNotCharging) == kBatteryStatusDischarging);
}

TEST_CASE("physicalBatteryStatusToWire: unknown maps to wire unknown", "[battery][mapping]") {
    REQUIRE(physicalBatteryStatusToWire(kAndroidStatusUnknown) == kBatteryStatusUnknown);
}

TEST_CASE("physicalBatteryStatusToWire: an out-of-range status falls back to unknown",
          "[battery][mapping]") {
    REQUIRE(physicalBatteryStatusToWire(99) == kBatteryStatusUnknown);
    REQUIRE(physicalBatteryStatusToWire(-1) == kBatteryStatusUnknown);
    REQUIRE(physicalBatteryStatusToWire(0) == kBatteryStatusUnknown);
}

TEST_CASE("physicalBatteryMapping: every mapped sample is accepted by the validator",
          "[battery][mapping]") {
    for (float cap = 0.0f; cap <= 1.0f; cap += 0.05f) {
        const auto s = physicalBatteryMapping(true, cap, kAndroidStatusDischarging);
        REQUIRE(s.has_value());
        REQUIRE(batterySampleValid(*s));
    }
    REQUIRE(batterySampleValid(*physicalBatteryMapping(true, std::nanf(""), kAndroidStatusFull)));
    REQUIRE(batterySampleValid(*physicalBatteryMapping(true, 2.0f, kAndroidStatusCharging)));
}
