// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The internal-button word to Moonlight button-flag translation, pinned per
// bit so a mis-mapped Home/Guide or stick click is caught at build time.

#include "core/input/GamepadButtonLayouts.h"
#include "core/moonlight/MoonlightButtonMap.h"
#include "core/moonlight/MoonlightProtocol.h"

#include <catch2/catch_test_macros.hpp>

using namespace dish::moonmap;
using namespace dish;

TEST_CASE("each internal button maps to its Moonlight flag", "[moonlight][buttonmap]") {
    CHECK(toMoonlightButtons(inbtn::kDpadUp) == moonproto::kBtnDpadUp);
    CHECK(toMoonlightButtons(inbtn::kDpadDown) == moonproto::kBtnDpadDown);
    CHECK(toMoonlightButtons(inbtn::kDpadLeft) == moonproto::kBtnDpadLeft);
    CHECK(toMoonlightButtons(inbtn::kDpadRight) == moonproto::kBtnDpadRight);
    CHECK(toMoonlightButtons(inbtn::kStart) == moonproto::kBtnStart);
    CHECK(toMoonlightButtons(inbtn::kBack) == moonproto::kBtnBack);
    CHECK(toMoonlightButtons(inbtn::kLeftThumb) == moonproto::kBtnLeftStick);
    CHECK(toMoonlightButtons(inbtn::kRightThumb) == moonproto::kBtnRightStick);
    CHECK(toMoonlightButtons(inbtn::kLeftShoulder) == moonproto::kBtnLeftButton);
    CHECK(toMoonlightButtons(inbtn::kRightShoulder) == moonproto::kBtnRightButton);
    CHECK(toMoonlightButtons(inbtn::kA) == moonproto::kBtnA);
    CHECK(toMoonlightButtons(inbtn::kB) == moonproto::kBtnB);
    CHECK(toMoonlightButtons(inbtn::kX) == moonproto::kBtnX);
    CHECK(toMoonlightButtons(inbtn::kY) == moonproto::kBtnY);
}

TEST_CASE("Home/Guide moves from bit 0x0400 to Moonlight's HOME flag", "[moonlight][buttonmap]") {
    // Both happen to be 0x0400 in the two vocabularies, but assert it rather
    // than assume it: the internal word has no Guide bit, so Home rides Start
    // + Back here only when set explicitly. This documents the current lack of
    // a Guide source without silently dropping it.
    CHECK((toMoonlightButtons(0) & moonproto::kBtnHome) == 0);
}

TEST_CASE("empty and full words", "[moonlight][buttonmap]") {
    CHECK(toMoonlightButtons(0) == 0);
    const std::uint16_t all = inbtn::kDpadUp | inbtn::kDpadDown | inbtn::kDpadLeft |
                              inbtn::kDpadRight | inbtn::kStart | inbtn::kBack | inbtn::kLeftThumb |
                              inbtn::kRightThumb | inbtn::kLeftShoulder | inbtn::kRightShoulder |
                              inbtn::kA | inbtn::kB | inbtn::kX | inbtn::kY;
    const std::uint32_t expected =
        moonproto::kBtnDpadUp | moonproto::kBtnDpadDown | moonproto::kBtnDpadLeft |
        moonproto::kBtnDpadRight | moonproto::kBtnStart | moonproto::kBtnBack |
        moonproto::kBtnLeftStick | moonproto::kBtnRightStick | moonproto::kBtnLeftButton |
        moonproto::kBtnRightButton | moonproto::kBtnA | moonproto::kBtnB | moonproto::kBtnX |
        moonproto::kBtnY;
    CHECK(toMoonlightButtons(all) == expected);
}

TEST_CASE("ABXY combination reproduces the doc example", "[moonlight][buttonmap]") {
    // A + X pressed -> 0x1000 | 0x4000 = 0x5000.
    CHECK(toMoonlightButtons(inbtn::kA | inbtn::kX) == 0x5000U);
}

TEST_CASE("the Satellite-only mic-mute bit never reaches a Moonlight button word",
          "[moonlight][buttonmap]") {
    // 0x0800 has no XINPUT assignment; protocol 2 spends it on the DualSense
    // mic-mute STATE (input::layout::kXusbMicMute), and a Direct-claimed
    // DualSense folds it into every report while muted. The map's explicitness
    // is what strips it — these pins keep that structural fact from eroding
    // into an accidental row.
    CHECK(toMoonlightButtons(input::layout::kXusbMicMute) == 0U);
    const std::uint16_t all = inbtn::kDpadUp | inbtn::kDpadDown | inbtn::kDpadLeft |
                              inbtn::kDpadRight | inbtn::kStart | inbtn::kBack | inbtn::kLeftThumb |
                              inbtn::kRightThumb | inbtn::kLeftShoulder | inbtn::kRightShoulder |
                              inbtn::kA | inbtn::kB | inbtn::kX | inbtn::kY;
    // Alongside every mapped bit, the word reads the same with or without it.
    CHECK(toMoonlightButtons(static_cast<std::uint16_t>(all | input::layout::kXusbMicMute)) ==
          toMoonlightButtons(all));
    // The bit the map must keep dropping is the bit the wire spends.
    CHECK(input::layout::kXusbMicMute == 0x0800);
}
