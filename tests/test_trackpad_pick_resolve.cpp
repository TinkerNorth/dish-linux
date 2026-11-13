// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The Steam slot's pick ladder: the stored pick where the model offers it, else
// the model's default, held to what the path can carry, and its projection onto
// the descriptor's touchpadMode.

#include "core/model/Protocol.h"
#include "core/reducer/TrackpadPickResolve.h"

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <vector>

namespace proto = dish::proto;
using dish::reducer::defaultTrackpadPick;
using dish::reducer::effectiveTrackpadPick;
using dish::reducer::isValidTrackpadPickName;
using dish::reducer::kTrackpadPickNames;
using dish::reducer::offeredTrackpadPicks;
using dish::reducer::SteamTrackpads;
using dish::reducer::TrackpadPick;
using dish::reducer::trackpadPickChoices;
using dish::reducer::trackpadPickFromName;
using dish::reducer::trackpadPickName;
using dish::reducer::wireModeForTrackpadPick;

namespace {
const std::optional<std::string> kNeverPicked = std::nullopt;
using Picks = std::vector<TrackpadPick>;
} // namespace

namespace {
std::string nameOf(TrackpadPick pick) { return std::string(trackpadPickName(pick)); }
} // namespace

TEST_CASE("the pick names are stick, pad, mouse, off in the enum's order",
          "[trackpad-pick][names]") {
    const std::vector<std::string> names(kTrackpadPickNames.begin(), kTrackpadPickNames.end());
    CHECK(names == std::vector<std::string>{"stick", "pad", "mouse", "off"});
    CHECK(nameOf(TrackpadPick::Stick) == "stick");
    CHECK(nameOf(TrackpadPick::Pad) == "pad");
    CHECK(nameOf(TrackpadPick::Mouse) == "mouse");
    CHECK(nameOf(TrackpadPick::Off) == "off");
    for (const std::string& name : names) {
        REQUIRE(trackpadPickFromName(name).has_value());
        CHECK(nameOf(*trackpadPickFromName(name)) == name);
        CHECK(isValidTrackpadPickName(name));
    }
    CHECK_FALSE(trackpadPickFromName("Stick").has_value());
    CHECK_FALSE(trackpadPickFromName("ds4").has_value());
    CHECK_FALSE(isValidTrackpadPickName(""));
}

TEST_CASE("the 2026 pad offers no stick pick: it has a real right stick",
          "[trackpad-pick][choices]") {
    CHECK(trackpadPickChoices(SteamTrackpads::Pad2015) ==
          Picks{TrackpadPick::Stick, TrackpadPick::Pad, TrackpadPick::Mouse, TrackpadPick::Off});
    CHECK(trackpadPickChoices(SteamTrackpads::Pad2026) ==
          Picks{TrackpadPick::Pad, TrackpadPick::Mouse, TrackpadPick::Off});
    CHECK(trackpadPickChoices(SteamTrackpads::None).empty());
}

TEST_CASE("a picker lists mouse only where this client can route a pad as the mouse",
          "[trackpad-pick][choices]") {
    CHECK(offeredTrackpadPicks(SteamTrackpads::Pad2015, false) ==
          Picks{TrackpadPick::Stick, TrackpadPick::Pad, TrackpadPick::Off});
    CHECK(offeredTrackpadPicks(SteamTrackpads::Pad2015, true) ==
          Picks{TrackpadPick::Stick, TrackpadPick::Pad, TrackpadPick::Mouse, TrackpadPick::Off});
    CHECK(offeredTrackpadPicks(SteamTrackpads::Pad2026, false) ==
          Picks{TrackpadPick::Pad, TrackpadPick::Off});
}

TEST_CASE("defaults: the 2015 pad starts on stick, the 2026 pad on pad when the type has a surface",
          "[trackpad-pick][default]") {
    CHECK(defaultTrackpadPick(SteamTrackpads::Pad2015, true) == TrackpadPick::Stick);
    CHECK(defaultTrackpadPick(SteamTrackpads::Pad2015, false) == TrackpadPick::Stick);
    CHECK(defaultTrackpadPick(SteamTrackpads::Pad2026, true) == TrackpadPick::Pad);
    CHECK(defaultTrackpadPick(SteamTrackpads::Pad2026, false) == TrackpadPick::Off);
    CHECK(defaultTrackpadPick(SteamTrackpads::None, true) == TrackpadPick::Off);
}

TEST_CASE("effective: a never-made pick is the model's default", "[trackpad-pick][effective]") {
    CHECK(effectiveTrackpadPick(kNeverPicked, SteamTrackpads::Pad2015, true, true) ==
          TrackpadPick::Stick);
    CHECK(effectiveTrackpadPick(kNeverPicked, SteamTrackpads::Pad2026, true, false) ==
          TrackpadPick::Pad);
    CHECK(effectiveTrackpadPick(kNeverPicked, SteamTrackpads::Pad2026, false, true) ==
          TrackpadPick::Off);
}

TEST_CASE("effective: a stored pick the model does not offer falls back to the default",
          "[trackpad-pick][effective]") {
    // The 2026 pad never takes stick; a 2015 store with a typo reads as never picked.
    CHECK(effectiveTrackpadPick(std::string("stick"), SteamTrackpads::Pad2026, true, true) ==
          TrackpadPick::Pad);
    CHECK(effectiveTrackpadPick(std::string("banana"), SteamTrackpads::Pad2015, true, true) ==
          TrackpadPick::Stick);
}

TEST_CASE(
    "effective: pad needs the type's surface and mouse the host's grant, else off, never the other",
    "[trackpad-pick][effective]") {
    CHECK(effectiveTrackpadPick(std::string("pad"), SteamTrackpads::Pad2015, true, false) ==
          TrackpadPick::Pad);
    CHECK(effectiveTrackpadPick(std::string("pad"), SteamTrackpads::Pad2015, false, true) ==
          TrackpadPick::Off);
    CHECK(effectiveTrackpadPick(std::string("mouse"), SteamTrackpads::Pad2026, false, true) ==
          TrackpadPick::Mouse);
    CHECK(effectiveTrackpadPick(std::string("mouse"), SteamTrackpads::Pad2026, true, false) ==
          TrackpadPick::Off);
    CHECK(effectiveTrackpadPick(std::string("stick"), SteamTrackpads::Pad2015, false, false) ==
          TrackpadPick::Stick);
    CHECK(effectiveTrackpadPick(std::string("off"), SteamTrackpads::Pad2015, true, true) ==
          TrackpadPick::Off);
}

TEST_CASE("the wire mode: pad is ds4, mouse is mouse, stick and off carry no surface",
          "[trackpad-pick][wire]") {
    CHECK(wireModeForTrackpadPick(TrackpadPick::Pad) == proto::kTouchpadModeDs4);
    CHECK(wireModeForTrackpadPick(TrackpadPick::Mouse) == proto::kTouchpadModeMouse);
    CHECK(wireModeForTrackpadPick(TrackpadPick::Stick) == proto::kTouchpadModeOff);
    CHECK(wireModeForTrackpadPick(TrackpadPick::Off) == proto::kTouchpadModeOff);
}
