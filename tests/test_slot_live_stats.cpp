// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "UI/SlotLiveStats.h"

#include <catch2/catch_test_macros.hpp>

using dish::models::SlotLiveRates;
using dish::ui::gamepadRateChip;
using dish::ui::motionRateChip;
using dish::ui::pollRateChip;
using dish::ui::RateChip;
using dish::ui::RateChipKind;

TEST_CASE("gamepadRateChip: a USB-direct pad with a live rate shows it live", "[slotlivestats]") {
    SlotLiveRates r;
    r.gamepadHz = 120;
    r.gamepadPeakHz = 250; // peak is higher, but Direct streams continuously
    const RateChip c = gamepadRateChip(r, /*usbDirect=*/true);
    REQUIRE(c.kind == RateChipKind::Live);
    REQUIRE(c.hz == 120);
}

TEST_CASE("gamepadRateChip: a routed pad shows the peak with a ~", "[slotlivestats]") {
    SlotLiveRates r;
    r.gamepadHz = 0; // idle right now (routed pads only deliver on actuation)
    r.gamepadPeakHz = 60;
    const RateChip c = gamepadRateChip(r, /*usbDirect=*/false);
    REQUIRE(c.kind == RateChipKind::Peak);
    REQUIRE(c.hz == 60);
}

TEST_CASE("gamepadRateChip: a routed pad with a live rate still shows the peak",
          "[slotlivestats]") {
    SlotLiveRates r;
    r.gamepadHz = 45;
    r.gamepadPeakHz = 60;
    const RateChip c = gamepadRateChip(r, /*usbDirect=*/false);
    REQUIRE(c.kind == RateChipKind::Peak);
    REQUIRE(c.hz == 60);
}

TEST_CASE("gamepadRateChip: nothing measured yet is hidden", "[slotlivestats]") {
    const RateChip direct = gamepadRateChip(SlotLiveRates{}, true);
    const RateChip routed = gamepadRateChip(SlotLiveRates{}, false);
    REQUIRE(direct.kind == RateChipKind::Hidden);
    REQUIRE(routed.kind == RateChipKind::Hidden);
}

TEST_CASE("gamepadRateChip: a USB-direct pad that is momentarily idle falls back to peak",
          "[slotlivestats]") {
    SlotLiveRates r;
    r.gamepadHz = 0;
    r.gamepadPeakHz = 250;
    const RateChip c = gamepadRateChip(r, /*usbDirect=*/true);
    REQUIRE(c.kind == RateChipKind::Peak);
    REQUIRE(c.hz == 250);
}

TEST_CASE("motionRateChip: a live gyro rate shows live (no ~)", "[slotlivestats]") {
    SlotLiveRates r;
    r.motionHz = 250;
    const RateChip c = motionRateChip(r);
    REQUIRE(c.kind == RateChipKind::Live);
    REQUIRE(c.hz == 250);
}

TEST_CASE("motionRateChip: an idle gyro is hidden", "[slotlivestats]") {
    SlotLiveRates r;
    r.motionHz = 0;
    r.motionPeakHz = 250; // a recorded peak does not resurrect the chip
    REQUIRE(motionRateChip(r).kind == RateChipKind::Hidden);
}

TEST_CASE("pollRateChip: shown live for a USB-direct pad with a measurement", "[slotlivestats]") {
    SlotLiveRates r;
    r.directPollHz = 1000;
    const RateChip c = pollRateChip(r, /*usbDirect=*/true);
    REQUIRE(c.kind == RateChipKind::Live);
    REQUIRE(c.hz == 1000);
}

TEST_CASE("pollRateChip: hidden for a non-direct pad even if a stale value lingers",
          "[slotlivestats]") {
    SlotLiveRates r;
    r.directPollHz = 1000; // would never be set for a routed pad, but be defensive
    REQUIRE(pollRateChip(r, /*usbDirect=*/false).kind == RateChipKind::Hidden);
}

TEST_CASE("pollRateChip: hidden for a direct pad with no measurement yet", "[slotlivestats]") {
    REQUIRE(pollRateChip(SlotLiveRates{}, /*usbDirect=*/true).kind == RateChipKind::Hidden);
}

TEST_CASE("SlotLiveRates::hasAny is false when every rate is zero", "[slotlivestats]") {
    REQUIRE_FALSE(SlotLiveRates{}.hasAny());
}

TEST_CASE("SlotLiveRates::hasAny trips on any populated rate, including directPollHz",
          "[slotlivestats]") {
    SlotLiveRates a;
    a.gamepadPeakHz = 60;
    REQUIRE(a.hasAny());
    SlotLiveRates b;
    b.motionHz = 250;
    REQUIRE(b.hasAny());
    SlotLiveRates c;
    c.directPollHz = 1000;
    REQUIRE(c.hasAny());
}
