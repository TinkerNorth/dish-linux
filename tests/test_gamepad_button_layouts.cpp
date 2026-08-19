// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The BTN_* / HAT_* literals below are the HID wire values, spelled out here
// rather than taken from the kHid* / kHat* constants under test.

#include "core/input/GamepadButtonLayouts.h"

#include <catch2/catch_test_macros.hpp>

namespace layout = dish::input::layout;

namespace {

constexpr int BTN_A = 0x0001;
constexpr int BTN_B = 0x0002;
constexpr int BTN_X = 0x0004;
constexpr int BTN_Y = 0x0008;
constexpr int BTN_LB = 0x0010;
constexpr int BTN_RB = 0x0020;
constexpr int BTN_SELECT = 0x0040;
constexpr int BTN_START = 0x0080;
constexpr int BTN_LS = 0x0100;
constexpr int BTN_RS = 0x0200;
constexpr int BTN_HOME = 0x0400;

constexpr int HAT_NONE = 0;
constexpr int HAT_N = 1;
constexpr int HAT_NE = 2;
constexpr int HAT_E = 3;
constexpr int HAT_SE = 4;
constexpr int HAT_S = 5;
constexpr int HAT_SW = 6;
constexpr int HAT_W = 7;
constexpr int HAT_NW = 8;

void assertHid(int wButtons, int expectedHid, int expectedHat) {
    const int packed = layout::xusbToHid(wButtons);
    REQUIRE(layout::hidButtonsOf(packed) == expectedHid);
    REQUIRE(layout::hidHatOf(packed) == expectedHat);
}

void assertXusb(int hidButtons, int hat, int expected) {
    REQUIRE(layout::hidToXusb(hidButtons, hat) == expected);
}

} // namespace

TEST_CASE("xusb A maps to HID BTN_A", "[input][layout]") { assertHid(0x1000, BTN_A, 0); }
TEST_CASE("xusb B maps to HID BTN_B", "[input][layout]") { assertHid(0x2000, BTN_B, 0); }
TEST_CASE("xusb X maps to HID BTN_X", "[input][layout]") { assertHid(0x4000, BTN_X, 0); }
TEST_CASE("xusb Y maps to HID BTN_Y", "[input][layout]") { assertHid(0x8000, BTN_Y, 0); }
TEST_CASE("xusb LB maps to HID BTN_LB", "[input][layout]") { assertHid(0x0100, BTN_LB, 0); }
TEST_CASE("xusb RB maps to HID BTN_RB", "[input][layout]") { assertHid(0x0200, BTN_RB, 0); }
TEST_CASE("xusb BACK maps to HID BTN_SELECT", "[input][layout]") {
    assertHid(0x0020, BTN_SELECT, 0);
}
TEST_CASE("xusb START maps to HID BTN_START", "[input][layout]") {
    assertHid(0x0010, BTN_START, 0);
}
TEST_CASE("xusb LEFT_THUMB maps to HID BTN_LS", "[input][layout]") { assertHid(0x0040, BTN_LS, 0); }
TEST_CASE("xusb RIGHT_THUMB maps to HID BTN_RS", "[input][layout]") {
    assertHid(0x0080, BTN_RS, 0);
}
TEST_CASE("xusb GUIDE maps to HID BTN_HOME", "[input][layout]") { assertHid(0x0400, BTN_HOME, 0); }

TEST_CASE("xusb dpad neutral maps to hat 0", "[input][layout]") { assertHid(0x0000, 0, HAT_NONE); }
TEST_CASE("xusb dpad up maps to hat N", "[input][layout]") { assertHid(0x0001, 0, HAT_N); }
TEST_CASE("xusb dpad down maps to hat S", "[input][layout]") { assertHid(0x0002, 0, HAT_S); }
TEST_CASE("xusb dpad left maps to hat W", "[input][layout]") { assertHid(0x0004, 0, HAT_W); }
TEST_CASE("xusb dpad right maps to hat E", "[input][layout]") { assertHid(0x0008, 0, HAT_E); }
TEST_CASE("xusb dpad up+right maps to hat NE", "[input][layout]") {
    assertHid(0x0001 | 0x0008, 0, HAT_NE);
}
TEST_CASE("xusb dpad down+right maps to hat SE", "[input][layout]") {
    assertHid(0x0002 | 0x0008, 0, HAT_SE);
}
TEST_CASE("xusb dpad down+left maps to hat SW", "[input][layout]") {
    assertHid(0x0002 | 0x0004, 0, HAT_SW);
}
TEST_CASE("xusb dpad up+left maps to hat NW", "[input][layout]") {
    assertHid(0x0001 | 0x0004, 0, HAT_NW);
}

TEST_CASE("xusb with A plus B plus X plus Y sets all four HID face bits", "[input][layout]") {
    const int packed = layout::xusbToHid(0x1000 | 0x2000 | 0x4000 | 0x8000);
    const int expected = BTN_A | BTN_B | BTN_X | BTN_Y;
    REQUIRE(layout::hidButtonsOf(packed) == expected);
    REQUIRE(layout::hidHatOf(packed) == HAT_NONE);
}

TEST_CASE("xusb zero is identity", "[input][layout]") {
    const int packed = layout::xusbToHid(0);
    REQUIRE(layout::hidButtonsOf(packed) == 0);
    REQUIRE(layout::hidHatOf(packed) == 0);
}

TEST_CASE("xusb unknown bits are dropped", "[input][layout]") {
    // 0x0800 is reserved/unused in XUSB.
    REQUIRE(layout::hidButtonsOf(layout::xusbToHid(0x0800)) == 0);
}

TEST_CASE("HID BTN_A maps to xusb A", "[input][layout]") { assertXusb(BTN_A, 0, 0x1000); }
TEST_CASE("HID BTN_B maps to xusb B", "[input][layout]") { assertXusb(BTN_B, 0, 0x2000); }
TEST_CASE("HID BTN_X maps to xusb X", "[input][layout]") { assertXusb(BTN_X, 0, 0x4000); }
TEST_CASE("HID BTN_Y maps to xusb Y", "[input][layout]") { assertXusb(BTN_Y, 0, 0x8000); }
TEST_CASE("HID BTN_LB maps to xusb LB", "[input][layout]") { assertXusb(BTN_LB, 0, 0x0100); }
TEST_CASE("HID BTN_RB maps to xusb RB", "[input][layout]") { assertXusb(BTN_RB, 0, 0x0200); }
TEST_CASE("HID BTN_SELECT maps to xusb BACK", "[input][layout]") {
    assertXusb(BTN_SELECT, 0, 0x0020);
}
TEST_CASE("HID BTN_START maps to xusb START", "[input][layout]") {
    assertXusb(BTN_START, 0, 0x0010);
}
TEST_CASE("HID BTN_LS maps to xusb LEFT_THUMB", "[input][layout]") {
    assertXusb(BTN_LS, 0, 0x0040);
}
TEST_CASE("HID BTN_RS maps to xusb RIGHT_THUMB", "[input][layout]") {
    assertXusb(BTN_RS, 0, 0x0080);
}
TEST_CASE("HID BTN_HOME maps to xusb GUIDE", "[input][layout]") { assertXusb(BTN_HOME, 0, 0x0400); }

TEST_CASE("HID hat 0 maps to xusb dpad neutral", "[input][layout]") {
    assertXusb(0, HAT_NONE, 0x0000);
}
TEST_CASE("HID hat N maps to xusb dpad up", "[input][layout]") { assertXusb(0, HAT_N, 0x0001); }
TEST_CASE("HID hat S maps to xusb dpad down", "[input][layout]") { assertXusb(0, HAT_S, 0x0002); }
TEST_CASE("HID hat W maps to xusb dpad left", "[input][layout]") { assertXusb(0, HAT_W, 0x0004); }
TEST_CASE("HID hat E maps to xusb dpad right", "[input][layout]") { assertXusb(0, HAT_E, 0x0008); }
TEST_CASE("HID hat NE maps to xusb dpad up+right", "[input][layout]") {
    assertXusb(0, HAT_NE, 0x0001 | 0x0008);
}
TEST_CASE("HID hat SE maps to xusb dpad down+right", "[input][layout]") {
    assertXusb(0, HAT_SE, 0x0002 | 0x0008);
}
TEST_CASE("HID hat SW maps to xusb dpad down+left", "[input][layout]") {
    assertXusb(0, HAT_SW, 0x0002 | 0x0004);
}
TEST_CASE("HID hat NW maps to xusb dpad up+left", "[input][layout]") {
    assertXusb(0, HAT_NW, 0x0001 | 0x0004);
}

TEST_CASE("xusbToHid then hidToXusb is identity for every canonical bit", "[input][layout]") {
    const int xusbBits[] = {0x0001, 0x0002, 0x0004, 0x0008, 0x0010, 0x0020, 0x0040, 0x0080,
                            0x0100, 0x0200, 0x0400, 0x1000, 0x2000, 0x4000, 0x8000};
    for (const int bit : xusbBits) {
        const int packed = layout::xusbToHid(bit);
        REQUIRE(layout::hidToXusb(layout::hidButtonsOf(packed), layout::hidHatOf(packed)) == bit);
    }
}
