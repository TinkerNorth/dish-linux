// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The trackpad router: both of a Steam Controller's pads onto the one wire
// surface, the 2015 right stick or nowhere, by the slot's pick. The pin the
// design asked for is the table at the end: over every pick and both models,
// each pad position lands in at most one wire field and each click in exactly
// one, so nothing double-actuates.

#include "core/input/SteamTrackpads.h"
#include "core/model/Protocol.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <set>
#include <string>

using namespace dish::input::usbparse;
namespace proto = dish::proto;

namespace {

PadSample pad(std::int16_t x, std::int16_t y, bool click = false) {
    PadSample p;
    p.active = true;
    p.x = x;
    p.y = y;
    p.click = click;
    return p;
}

PadSample lifted(bool click = false) {
    PadSample p;
    p.click = click;
    return p;
}

SteamTrackpadRoute route(TrackpadPick pick, SteamTrackpads model, const SteamPads& pads) {
    PadScrollTracker scroll;
    return routeSteamTrackpads(pick, model, pads, scroll);
}

} // namespace

TEST_CASE("the router's wheel notch and finger ids are the wire's", "[steam-trackpads]") {
    CHECK(kPadScrollWheelNotch == proto::kScrollUnitsPerNotch);
    CHECK(kLeftPadFingerId == 0);
    CHECK(kRightPadFingerId == 1);
    CHECK(kPadHalfSurface == 16384);
}

TEST_CASE(
    "stick: the 2015 right pad is the right stick and its click R3, the left click rides the word",
    "[steam-trackpads]") {
    const auto r = route(TrackpadPick::Stick, SteamTrackpads::Pad2015,
                         {pad(100, 200, true), pad(-3000, 4000, true)});
    CHECK(r.rightStickX == -3000);
    CHECK(r.rightStickY == 4000);
    CHECK(r.rightStickClick);
    CHECK(r.extButtons == extbtn::kLeftPadClick);
    CHECK_FALSE(r.finger0Active);
    CHECK_FALSE(r.finger1Active);
    CHECK_FALSE(r.click);
    CHECK_FALSE(r.rightClick);
    CHECK(r.scrollV == 0);
}

TEST_CASE("stick: a lifted 2015 right pad recentres the right stick", "[steam-trackpads]") {
    const auto r = route(TrackpadPick::Stick, SteamTrackpads::Pad2015, {lifted(), lifted(true)});
    CHECK(r.rightStickX == 0);
    CHECK(r.rightStickY == 0);
    CHECK(r.rightStickClick);
}

TEST_CASE("stick on the 2026 pad reads as off: a real right stick is nobody's to take",
          "[steam-trackpads]") {
    const auto r = route(TrackpadPick::Stick, SteamTrackpads::Pad2026,
                         {pad(100, 200, true), pad(-3000, 4000, true)});
    CHECK(r.rightStickX == 0);
    CHECK(r.rightStickY == 0);
    CHECK_FALSE(r.rightStickClick);
    CHECK(r.extButtons == (extbtn::kLeftPadClick | extbtn::kRightPadClick));
    CHECK_FALSE(r.finger0Active);
    CHECK_FALSE(r.finger1Active);
}

TEST_CASE(
    "pad: the left pad is finger 0 on the left half, the right pad finger 1 on the right half",
    "[steam-trackpads]") {
    const auto r =
        route(TrackpadPick::Pad, SteamTrackpads::Pad2015, {pad(-32768, 32767), pad(32767, -32768)});
    CHECK(r.finger0Active);
    CHECK(r.finger0Id == kLeftPadFingerId);
    CHECK(r.finger0X == -32768);
    CHECK(r.finger0Y == -32767);
    CHECK(r.finger1Active);
    CHECK(r.finger1Id == kRightPadFingerId);
    CHECK(r.finger1X == 32767);
    CHECK(r.finger1Y == 32767);
    // The halves meet in the middle: a centred pad sits a quarter in from its edge.
    const auto centred = route(TrackpadPick::Pad, SteamTrackpads::Pad2026, {pad(0, 0), pad(0, 0)});
    CHECK(centred.finger0X == -16384);
    CHECK(centred.finger1X == 16384);
    CHECK(centred.finger0Y == 0);
}

TEST_CASE("pad: either click is the surface's click, the stick stays centred and no bit is set",
          "[steam-trackpads]") {
    const auto left =
        route(TrackpadPick::Pad, SteamTrackpads::Pad2015, {pad(1, 2, true), pad(3, 4)});
    CHECK(left.click);
    const auto right =
        route(TrackpadPick::Pad, SteamTrackpads::Pad2015, {pad(1, 2), pad(3, 4, true)});
    CHECK(right.click);
    CHECK(right.rightStickX == 0);
    CHECK(right.rightStickY == 0);
    CHECK_FALSE(right.rightStickClick);
    CHECK(right.extButtons == 0);
    CHECK_FALSE(right.rightClick);
    const auto none = route(TrackpadPick::Pad, SteamTrackpads::Pad2015, {lifted(), lifted()});
    CHECK_FALSE(none.click);
    CHECK_FALSE(none.finger0Active);
    CHECK_FALSE(none.finger1Active);
}

TEST_CASE(
    "mouse: the right pad is the cursor and the left button, the left pad's click the right button",
    "[steam-trackpads]") {
    const auto r = route(TrackpadPick::Mouse, SteamTrackpads::Pad2026,
                         {pad(500, 600, true), pad(-7000, 8000, true)});
    CHECK(r.finger0Active);
    CHECK(r.finger0Id == kLeftPadFingerId);
    CHECK(r.finger0X == -7000);
    CHECK(r.finger0Y == -8000);
    CHECK_FALSE(r.finger1Active);
    CHECK(r.click);
    CHECK(r.rightClick);
    CHECK(r.extButtons == 0);
    CHECK(r.rightStickX == 0);
    CHECK_FALSE(r.rightStickClick);
}

TEST_CASE(
    "mouse: the left pad's vertical travel is the wheel, a notch per 4096 units, remainder carried",
    "[steam-trackpads]") {
    PadScrollTracker scroll;
    // The first touch only arms the tracker.
    CHECK(routeSteamTrackpads(TrackpadPick::Mouse, SteamTrackpads::Pad2015, {pad(0, 0), lifted()},
                              scroll)
              .scrollV == 0);
    CHECK(routeSteamTrackpads(TrackpadPick::Mouse, SteamTrackpads::Pad2015,
                              {pad(0, 4096), lifted()}, scroll)
              .scrollV == kPadScrollWheelNotch);
    // 6000 more: one notch, 1904 carried.
    CHECK(routeSteamTrackpads(TrackpadPick::Mouse, SteamTrackpads::Pad2015,
                              {pad(0, 10096), lifted()}, scroll)
              .scrollV == kPadScrollWheelNotch);
    // 2192 more completes the carried notch.
    CHECK(routeSteamTrackpads(TrackpadPick::Mouse, SteamTrackpads::Pad2015,
                              {pad(0, 12288), lifted()}, scroll)
              .scrollV == kPadScrollWheelNotch);
    // Down the pad is a negative notch.
    CHECK(routeSteamTrackpads(TrackpadPick::Mouse, SteamTrackpads::Pad2015,
                              {pad(0, 4096), lifted()}, scroll)
              .scrollV == static_cast<std::int16_t>(-2 * kPadScrollWheelNotch));
    // A lifted pad starts the next swipe fresh: no notch from the jump back.
    CHECK(routeSteamTrackpads(TrackpadPick::Mouse, SteamTrackpads::Pad2015, {lifted(), lifted()},
                              scroll)
              .scrollV == 0);
    CHECK(routeSteamTrackpads(TrackpadPick::Mouse, SteamTrackpads::Pad2015,
                              {pad(0, -30000), lifted()}, scroll)
              .scrollV == 0);
}

TEST_CASE("off: both pads vanish and both clicks ride the word", "[steam-trackpads]") {
    const auto r = route(TrackpadPick::Off, SteamTrackpads::Pad2015,
                         {pad(100, 200, true), pad(-3000, 4000, true)});
    CHECK(r.extButtons == (extbtn::kLeftPadClick | extbtn::kRightPadClick));
    CHECK_FALSE(r.finger0Active);
    CHECK_FALSE(r.finger1Active);
    CHECK_FALSE(r.click);
    CHECK_FALSE(r.rightClick);
    CHECK(r.rightStickX == 0);
    CHECK(r.rightStickY == 0);
    CHECK_FALSE(r.rightStickClick);
    CHECK(r.scrollV == 0);
}

namespace {

// Where one input landed, as the wire would see it.
std::set<std::string> fieldsOf(const SteamTrackpadRoute& r) {
    std::set<std::string> out;
    if (r.rightStickX != 0 || r.rightStickY != 0) { out.insert("rightStick"); }
    if (r.rightStickClick) { out.insert("r3"); }
    if (r.finger0Active) { out.insert("finger0"); }
    if (r.finger1Active) { out.insert("finger1"); }
    if (r.click) { out.insert("leftButton"); }
    if (r.rightClick) { out.insert("rightButton"); }
    if (r.scrollV != 0) { out.insert("wheel"); }
    if ((r.extButtons & extbtn::kLeftPadClick) != 0) { out.insert("leftPadClickBit"); }
    if ((r.extButtons & extbtn::kRightPadClick) != 0) { out.insert("rightPadClickBit"); }
    return out;
}

struct Expectation {
    TrackpadPick pick;
    SteamTrackpads model;
    std::set<std::string> leftPosition;
    std::set<std::string> rightPosition;
    std::set<std::string> leftClick;
    std::set<std::string> rightClick;
};

// A wheel needs two samples; the position probe moves the pad a notch so the
// mouse row shows where travel lands.
std::set<std::string> leftPositionFields(TrackpadPick pick, SteamTrackpads model) {
    PadScrollTracker scroll;
    const SteamPads first{pad(1000, 1000), lifted()};
    const SteamPads second{pad(1000, 1000 + kPadScrollUnitsPerNotch), lifted()};
    (void)routeSteamTrackpads(pick, model, first, scroll);
    return fieldsOf(routeSteamTrackpads(pick, model, second, scroll));
}

} // namespace

TEST_CASE("each position lands in at most one wire field and each click in exactly one, over "
          "every pick and model",
          "[steam-trackpads][invariant]") {
    const Expectation table[] = {
        {TrackpadPick::Stick,
         SteamTrackpads::Pad2015,
         {},
         {"rightStick"},
         {"leftPadClickBit"},
         {"r3"}},
        {TrackpadPick::Pad,
         SteamTrackpads::Pad2015,
         {"finger0"},
         {"finger1"},
         {"leftButton"},
         {"leftButton"}},
        {TrackpadPick::Mouse,
         SteamTrackpads::Pad2015,
         {"wheel"},
         {"finger0"},
         {"rightButton"},
         {"leftButton"}},
        {TrackpadPick::Off,
         SteamTrackpads::Pad2015,
         {},
         {},
         {"leftPadClickBit"},
         {"rightPadClickBit"}},
        {TrackpadPick::Stick,
         SteamTrackpads::Pad2026,
         {},
         {},
         {"leftPadClickBit"},
         {"rightPadClickBit"}},
        {TrackpadPick::Pad,
         SteamTrackpads::Pad2026,
         {"finger0"},
         {"finger1"},
         {"leftButton"},
         {"leftButton"}},
        {TrackpadPick::Mouse,
         SteamTrackpads::Pad2026,
         {"wheel"},
         {"finger0"},
         {"rightButton"},
         {"leftButton"}},
        {TrackpadPick::Off,
         SteamTrackpads::Pad2026,
         {},
         {},
         {"leftPadClickBit"},
         {"rightPadClickBit"}},
    };
    for (const Expectation& row : table) {
        INFO("pick " << static_cast<int>(row.pick) << " model " << static_cast<int>(row.model));
        const auto leftPosition = leftPositionFields(row.pick, row.model);
        const auto rightPosition =
            fieldsOf(route(row.pick, row.model, {lifted(), pad(1000, 1000)}));
        const auto leftClick = fieldsOf(route(row.pick, row.model, {lifted(true), lifted()}));
        const auto rightClick = fieldsOf(route(row.pick, row.model, {lifted(), lifted(true)}));
        CHECK(leftPosition == row.leftPosition);
        CHECK(rightPosition == row.rightPosition);
        CHECK(leftClick == row.leftClick);
        CHECK(rightClick == row.rightClick);
        CHECK(leftPosition.size() <= 1);
        CHECK(rightPosition.size() <= 1);
        CHECK(leftClick.size() == 1);
        CHECK(rightClick.size() == 1);
    }
}

TEST_CASE("outside stick the 2015 right stick stays centred and nothing is R3",
          "[steam-trackpads][invariant]") {
    for (const TrackpadPick pick : {TrackpadPick::Pad, TrackpadPick::Mouse, TrackpadPick::Off}) {
        const auto r =
            route(pick, SteamTrackpads::Pad2015, {pad(100, 200, true), pad(-3000, 4000, true)});
        CHECK(r.rightStickX == 0);
        CHECK(r.rightStickY == 0);
        CHECK_FALSE(r.rightStickClick);
    }
}
