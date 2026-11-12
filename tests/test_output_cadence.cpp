// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// What a claimed pad's output worker owes it on a clock. The periods are SDL's
// (SDL_hidapi_steam_triton.c): the lizard-mode settings every 3 s while the pad
// is open, a running rumble every 40 ms because the haptics stop on their own
// about 50 ms after a write.

#include "core/input/OutputCadence.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <optional>

using namespace std::chrono_literals;
using dish::input::OutputCadence;
using dish::input::usbparse::HidParser;

namespace {

const OutputCadence::Clock::time_point kClaimed = OutputCadence::Clock::time_point{} + 10s;

} // namespace

TEST_CASE("a family with nothing on a clock owes nothing", "[output-cadence]") {
    for (const HidParser p : {HidParser::DualSense, HidParser::DualShock4, HidParser::SwitchProUsb,
                              HidParser::SteamController, HidParser::GenericHid, HidParser::None}) {
        OutputCadence cadence(p, kClaimed);
        CHECK_FALSE(cadence.nextDue().has_value());
        CHECK_FALSE(cadence.settingsDue(kClaimed + 1h));
        cadence.rumbleSent(kClaimed, /*running=*/true);
        CHECK_FALSE(cadence.rumbleDue(kClaimed + 1h));
        CHECK_FALSE(cadence.nextDue().has_value());
    }
}

TEST_CASE("only the 2026 Steam Controller's settings ride the clock", "[output-cadence]") {
    const auto now = OutputCadence::Clock::now();
    CHECK(OutputCadence(HidParser::SteamTriton, now).sendsSettings());
    for (const HidParser p :
         {HidParser::SteamController, HidParser::DualSense, HidParser::DualShock4,
          HidParser::SwitchProUsb, HidParser::GenericHid, HidParser::None}) {
        CHECK_FALSE(OutputCadence(p, now).sendsSettings());
    }
}

TEST_CASE("the 2026 pad's settings are owed at the claim, then 3 s after each send",
          "[output-cadence][triton]") {
    // The claim sends no settings of its own, so the worker's first pass does.
    OutputCadence cadence(HidParser::SteamTriton, kClaimed);
    CHECK(cadence.settingsDue(kClaimed));
    REQUIRE(cadence.nextDue().has_value());
    CHECK(*cadence.nextDue() == kClaimed);

    const auto sentAt = kClaimed + 2ms;
    cadence.settingsSent(sentAt);
    CHECK_FALSE(cadence.settingsDue(sentAt + 2999ms));
    CHECK(cadence.settingsDue(sentAt + 3s));
    REQUIRE(cadence.nextDue().has_value());
    CHECK(*cadence.nextDue() == sentAt + 3s);
}

TEST_CASE("a running 2026 rumble is owed again 40 ms after each write",
          "[output-cadence][triton]") {
    OutputCadence cadence(HidParser::SteamTriton, kClaimed);
    cadence.settingsSent(kClaimed);
    CHECK_FALSE(cadence.rumbleDue(kClaimed + 1h));

    const auto firstWrite = kClaimed + 100ms;
    cadence.rumbleSent(firstWrite, /*running=*/true);
    CHECK_FALSE(cadence.rumbleDue(firstWrite + 39ms));
    CHECK(cadence.rumbleDue(firstWrite + 40ms));
    REQUIRE(cadence.nextDue().has_value());
    CHECK(*cadence.nextDue() == firstWrite + 40ms);

    // A re-send, or a new host write, starts the 40 ms over.
    const auto resend = firstWrite + 41ms;
    cadence.rumbleSent(resend, /*running=*/true);
    CHECK_FALSE(cadence.rumbleDue(resend + 39ms));
    CHECK(cadence.rumbleDue(resend + 40ms));
}

TEST_CASE("a stopped 2026 rumble is sent once and not repeated", "[output-cadence][triton]") {
    OutputCadence cadence(HidParser::SteamTriton, kClaimed);
    cadence.settingsSent(kClaimed);
    cadence.rumbleSent(kClaimed + 10ms, /*running=*/true);
    cadence.rumbleSent(kClaimed + 20ms, /*running=*/false);
    CHECK_FALSE(cadence.rumbleDue(kClaimed + 1s));
    // What is left is the settings, 3 s after they went.
    REQUIRE(cadence.nextDue().has_value());
    CHECK(*cadence.nextDue() == kClaimed + 3s);
}

TEST_CASE("the next deadline is the earlier of the two", "[output-cadence][triton]") {
    OutputCadence cadence(HidParser::SteamTriton, kClaimed);
    cadence.settingsSent(kClaimed);
    cadence.rumbleSent(kClaimed + 2990ms, /*running=*/true);
    REQUIRE(cadence.nextDue().has_value());
    CHECK(*cadence.nextDue() == kClaimed + 3s);
    cadence.settingsSent(kClaimed + 3s);
    REQUIRE(cadence.nextDue().has_value());
    CHECK(*cadence.nextDue() == kClaimed + 2990ms + 40ms);
}
