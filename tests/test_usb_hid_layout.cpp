// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The shared descriptor-driven decode rules, pinned against dish-android's
// usb_hid_descriptor_test.cpp fixtures byte for byte. parseReportDescriptor is
// the canonical constructor these vectors exercise; production builds the same
// field map HidrawGateway builds from the real descriptor, whose scaling and
// button-index mapping are the very functions under test here.

#include "core/input/UsbHidLayout.h"

#include "core/input/UsbReportParsers.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

using dish::input::usbhid::decodeFromLayout;
using dish::input::usbhid::HidLayout;
using dish::input::usbhid::parseReportDescriptor;
using dish::input::usbparse::ParsedReport;
namespace layout = dish::input::layout;

namespace {

constexpr std::uint16_t kDpadMask =
    layout::kXusbDpadUp | layout::kXusbDpadDown | layout::kXusbDpadLeft | layout::kXusbDpadRight;

// A standard two-stick gamepad: X/Y/Z/Rz (bytes 0-3), 4-bit hat + 4-bit pad
// (byte 4), 10 buttons + 6-bit pad (bytes 5-6). 56-bit / 7-byte input report,
// no report id.
const std::uint8_t kGamepadDescriptor[] = {
    0x05, 0x01,       // Usage Page (Generic Desktop)
    0x09, 0x05,       // Usage (Game Pad)
    0xA1, 0x01,       // Collection (Application)
    0x05, 0x01,       //   Usage Page (Generic Desktop)
    0x09, 0x30,       //   Usage (X)
    0x09, 0x31,       //   Usage (Y)
    0x09, 0x32,       //   Usage (Z)
    0x09, 0x35,       //   Usage (Rz)
    0x15, 0x00,       //   Logical Minimum (0)
    0x26, 0xFF, 0x00, //   Logical Maximum (255)
    0x75, 0x08,       //   Report Size (8)
    0x95, 0x04,       //   Report Count (4)
    0x81, 0x02,       //   Input (Data,Var,Abs)
    0x09, 0x39,       //   Usage (Hat switch)
    0x15, 0x00,       //   Logical Minimum (0)
    0x25, 0x07,       //   Logical Maximum (7)
    0x75, 0x04,       //   Report Size (4)
    0x95, 0x01,       //   Report Count (1)
    0x81, 0x42,       //   Input (Data,Var,Abs,Null)
    0x75, 0x04,       //   Report Size (4)
    0x95, 0x01,       //   Report Count (1)
    0x81, 0x01,       //   Input (Const)
    0x05, 0x09,       //   Usage Page (Button)
    0x19, 0x01,       //   Usage Minimum (1)
    0x29, 0x0A,       //   Usage Maximum (10)
    0x15, 0x00,       //   Logical Minimum (0)
    0x25, 0x01,       //   Logical Maximum (1)
    0x75, 0x01,       //   Report Size (1)
    0x95, 0x0A,       //   Report Count (10)
    0x81, 0x02,       //   Input (Data,Var,Abs)
    0x75, 0x01,       //   Report Size (1)
    0x95, 0x06,       //   Report Count (6)
    0x81, 0x01,       //   Input (Const)
    0xC0,             // End Collection
};

// Minimal X/Y gamepad behind Report ID 3: report is {0x03, X, Y}.
const std::uint8_t kReportIdDescriptor[] = {
    0x05, 0x01,       // Usage Page (Generic Desktop)
    0x09, 0x05,       // Usage (Game Pad)
    0xA1, 0x01,       // Collection (Application)
    0x85, 0x03,       //   Report ID (3)
    0x09, 0x30,       //   Usage (X)
    0x09, 0x31,       //   Usage (Y)
    0x15, 0x00,       //   Logical Minimum (0)
    0x26, 0xFF, 0x00, //   Logical Maximum (255)
    0x75, 0x08,       //   Report Size (8)
    0x95, 0x02,       //   Report Count (2)
    0x81, 0x02,       //   Input (Data,Var,Abs)
    0xC0,             // End Collection
};

// A single 32-bit X axis with a 31-bit logical max, to exercise wide-axis
// scaling.
const std::uint8_t kWideAxisDescriptor[] = {
    0x05, 0x01,                   // Usage Page (Generic Desktop)
    0x09, 0x05,                   // Usage (Game Pad)
    0xA1, 0x01,                   // Collection (Application)
    0x09, 0x30,                   //   Usage (X)
    0x15, 0x00,                   //   Logical Minimum (0)
    0x27, 0xFF, 0xFF, 0xFF, 0x7F, //   Logical Maximum (0x7FFFFFFF)
    0x75, 0x20,                   //   Report Size (32)
    0x95, 0x01,                   //   Report Count (1)
    0x81, 0x02,                   //   Input (Data,Var,Abs)
    0xC0,                         // End Collection
};

// A 4-direction hat (logical 0..3); raw 4 is the out-of-range null value.
const std::uint8_t kNarrowHatDescriptor[] = {
    0x05, 0x01, // Usage Page (Generic Desktop)
    0x09, 0x05, // Usage (Game Pad)
    0xA1, 0x01, // Collection (Application)
    0x09, 0x39, //   Usage (Hat switch)
    0x15, 0x00, //   Logical Minimum (0)
    0x25, 0x03, //   Logical Maximum (3)
    0x75, 0x08, //   Report Size (8)
    0x95, 0x01, //   Report Count (1)
    0x81, 0x02, //   Input (Data,Var,Abs)
    0xC0,       // End Collection
};

} // namespace

TEST_CASE("hid layout parses a standard gamepad descriptor", "[usb-hid-layout]") {
    HidLayout L;
    REQUIRE(parseReportDescriptor(kGamepadDescriptor, sizeof(kGamepadDescriptor), L));
    CHECK(L.valid);
    CHECK(L.reportId == 0);

    CHECK(L.lx.present);
    CHECK(L.lx.bitOffset == 0);
    CHECK(L.lx.bitSize == 8);
    CHECK(L.lx.logicalMax == 255);
    CHECK(L.ly.bitOffset == 8);
    CHECK(L.rx.bitOffset == 16); // Z
    CHECK(L.ry.bitOffset == 24); // Rz

    CHECK(L.hasHat);
    CHECK(L.hatBitOffset == 32);
    CHECK(L.hatBitSize == 4);
    CHECK(L.hatLogicalMax == 7);

    // Button block starts after the hat nibble + its 4-bit pad (byte 5, bit 40).
    CHECK(L.buttonBitOffset == 40);
    CHECK(L.buttonCount == 10);
}

TEST_CASE("hid layout decodes sticks buttons and hat", "[usb-hid-layout]") {
    HidLayout L;
    REQUIRE(parseReportDescriptor(kGamepadDescriptor, sizeof(kGamepadDescriptor), L));

    std::vector<std::uint8_t> report(7, 0);
    report[0] = 0xFF; // X full right
    report[4] = 0x02; // hat = 2 (East) in low nibble
    report[5] = 0x03; // buttons 1 and 2 (A, B)

    ParsedReport s{};
    REQUIRE(decodeFromLayout(report.data(), report.size(), s, L));
    CHECK(s.lx > 30000);
    CHECK((s.wButtons & layout::kXusbA) != 0);
    CHECK((s.wButtons & layout::kXusbB) != 0);
    CHECK((s.wButtons & layout::kXusbDpadRight) != 0);
}

TEST_CASE("hid layout detects and honors a report id", "[usb-hid-layout]") {
    HidLayout L;
    REQUIRE(parseReportDescriptor(kReportIdDescriptor, sizeof(kReportIdDescriptor), L));
    CHECK(L.reportId == 3);
    CHECK(L.lx.present);
    CHECK(L.lx.bitOffset == 0); // offsets are relative to the post-id payload

    std::vector<std::uint8_t> good = {0x03, 0xFF, 0x80};
    ParsedReport s{};
    REQUIRE(decodeFromLayout(good.data(), good.size(), s, L));
    CHECK(s.lx > 30000);

    std::vector<std::uint8_t> wrongId = {0x05, 0xFF, 0x80};
    ParsedReport s2{};
    CHECK_FALSE(decodeFromLayout(wrongId.data(), wrongId.size(), s2, L));
}

TEST_CASE("hid layout rejects a non-gamepad descriptor", "[usb-hid-layout]") {
    // Usage Page (Vendor), one byte of input: nothing gamepad-like.
    const std::uint8_t vendor[] = {0x06, 0x00, 0xFF, 0x09, 0x01, 0xA1, 0x01,
                                   0x75, 0x08, 0x95, 0x01, 0x81, 0x02, 0xC0};
    HidLayout L;
    CHECK_FALSE(parseReportDescriptor(vendor, sizeof(vendor), L));
    CHECK_FALSE(L.valid);
}

TEST_CASE("hid layout: empty descriptor is invalid and decode on invalid returns false",
          "[usb-hid-layout]") {
    HidLayout L;
    CHECK_FALSE(parseReportDescriptor(nullptr, 0, L));
    CHECK_FALSE(L.valid);

    std::vector<std::uint8_t> report(8, 0x7F);
    ParsedReport s{};
    CHECK_FALSE(decodeFromLayout(report.data(), report.size(), s, L));
}

TEST_CASE("hid layout: truncated descriptor does not overrun", "[usb-hid-layout]") {
    // A prefix that promises 2 data bytes but supplies none must not read past
    // the buffer.
    const std::uint8_t truncated[] = {0x26};
    HidLayout L;
    CHECK_FALSE(parseReportDescriptor(truncated, sizeof(truncated), L));
}

TEST_CASE("hid layout: wide axis scales without overflow", "[usb-hid-layout]") {
    HidLayout L;
    REQUIRE(parseReportDescriptor(kWideAxisDescriptor, sizeof(kWideAxisDescriptor), L));
    REQUIRE(L.lx.present);
    CHECK(L.lx.bitSize == 32);

    std::vector<std::uint8_t> full = {0xFF, 0xFF, 0xFF, 0x7F}; // raw 0x7FFFFFFF
    ParsedReport s{};
    REQUIRE(decodeFromLayout(full.data(), full.size(), s, L));
    CHECK(s.lx > 30000); // clamps near +max instead of wrapping to garbage
}

TEST_CASE("hid layout: narrow hat rejects the out-of-range null", "[usb-hid-layout]") {
    HidLayout L;
    REQUIRE(parseReportDescriptor(kNarrowHatDescriptor, sizeof(kNarrowHatDescriptor), L));
    REQUIRE(L.hasHat);
    CHECK(L.hatLogicalMax == 3);

    std::vector<std::uint8_t> east = {0x02}; // a real direction (East)
    ParsedReport s1{};
    REQUIRE(decodeFromLayout(east.data(), east.size(), s1, L));
    CHECK((s1.wButtons & layout::kXusbDpadRight) != 0);

    std::vector<std::uint8_t> nullDir = {0x04}; // out of 0..3 range: no direction
    ParsedReport s2{};
    REQUIRE(decodeFromLayout(nullDir.data(), nullDir.size(), s2, L));
    CHECK((s2.wButtons & kDpadMask) == 0);
}

// ── Switch-order remap (PDP wired Switch pads, dish-android #159) ────────────

namespace {

// PDP Faceoff Wired Pro (0e6f:0180) report shape: 14 buttons in Switch usage
// order (Y B A X L R ZL ZR Minus Plus L3 R3 Home Capture) + 2-bit pad, 4-bit
// hat + 4-bit pad, then X/Y/Z/Rz bytes. 56-bit / 7-byte input report, no
// report id.
const std::uint8_t kSwitchOrderDescriptor[] = {
    0x05, 0x01,       // Usage Page (Generic Desktop)
    0x09, 0x05,       // Usage (Game Pad)
    0xA1, 0x01,       // Collection (Application)
    0x15, 0x00,       //   Logical Minimum (0)
    0x25, 0x01,       //   Logical Maximum (1)
    0x75, 0x01,       //   Report Size (1)
    0x95, 0x0E,       //   Report Count (14)
    0x05, 0x09,       //   Usage Page (Button)
    0x19, 0x01,       //   Usage Minimum (1)
    0x29, 0x0E,       //   Usage Maximum (14)
    0x81, 0x02,       //   Input (Data,Var,Abs)
    0x95, 0x02,       //   Report Count (2)
    0x81, 0x01,       //   Input (Const)
    0x05, 0x01,       //   Usage Page (Generic Desktop)
    0x25, 0x07,       //   Logical Maximum (7)
    0x75, 0x04,       //   Report Size (4)
    0x95, 0x01,       //   Report Count (1)
    0x09, 0x39,       //   Usage (Hat switch)
    0x81, 0x42,       //   Input (Data,Var,Abs,Null)
    0x95, 0x01,       //   Report Count (1)
    0x81, 0x01,       //   Input (Const)
    0x26, 0xFF, 0x00, //   Logical Maximum (255)
    0x09, 0x30,       //   Usage (X)
    0x09, 0x31,       //   Usage (Y)
    0x09, 0x32,       //   Usage (Z)
    0x09, 0x35,       //   Usage (Rz)
    0x75, 0x08,       //   Report Size (8)
    0x95, 0x04,       //   Report Count (4)
    0x81, 0x02,       //   Input (Data,Var,Abs)
    0xC0,             // End Collection
};

std::vector<std::uint8_t> switchReport(std::uint8_t btnLo, std::uint8_t btnHi,
                                       std::uint8_t hat = 0x08, std::uint8_t x = 0x7F,
                                       std::uint8_t y = 0x7F, std::uint8_t z = 0x7F,
                                       std::uint8_t rz = 0x7F) {
    return {btnLo, btnHi, hat, x, y, z, rz};
}

HidLayout switchLayout(bool switchOrder) {
    HidLayout L;
    REQUIRE(parseReportDescriptor(kSwitchOrderDescriptor, sizeof(kSwitchOrderDescriptor), L));
    L.switchOrderButtons = switchOrder;
    return L;
}

} // namespace

TEST_CASE("switch-order: the Faceoff report shape parses to the expected field map",
          "[usb-hid-layout][switch-order]") {
    HidLayout L;
    REQUIRE(parseReportDescriptor(kSwitchOrderDescriptor, sizeof(kSwitchOrderDescriptor), L));
    CHECK(L.reportId == 0);
    CHECK(L.buttonBitOffset == 0);
    CHECK(L.buttonCount == 14);
    CHECK(L.hasHat);
    CHECK(L.hatBitOffset == 16);
    CHECK(L.hatBitSize == 4);
    CHECK(L.lx.bitOffset == 24);
    CHECK(L.ly.bitOffset == 32);
    CHECK(L.rx.bitOffset == 40); // Z
    CHECK(L.ry.bitOffset == 48); // Rz
    CHECK_FALSE(L.lt.present);
    CHECK_FALSE(L.rt.present);
}

TEST_CASE("switch-order: parse resets the order flag so attach must set it after",
          "[usb-hid-layout][switch-order]") {
    HidLayout L;
    L.switchOrderButtons = true;
    REQUIRE(parseReportDescriptor(kSwitchOrderDescriptor, sizeof(kSwitchOrderDescriptor), L));
    CHECK_FALSE(L.switchOrderButtons);
}

TEST_CASE("switch-order: western decode scrambles the Faceoff pad",
          "[usb-hid-layout][switch-order]") {
    // Pre-quirk behavior pin: without the catalog flag, physical A (bit 2)
    // lands on X, ZL lands on Back with no trigger, and R3/Home/Capture vanish.
    HidLayout L = switchLayout(false);

    ParsedReport a{};
    auto physicalA = switchReport(0x04, 0x00);
    REQUIRE(decodeFromLayout(physicalA.data(), physicalA.size(), a, L));
    CHECK(a.wButtons == layout::kXusbX);

    ParsedReport zl{};
    auto zlReport = switchReport(0x40, 0x00);
    REQUIRE(decodeFromLayout(zlReport.data(), zlReport.size(), zl, L));
    CHECK(zl.wButtons == layout::kXusbBack);
    CHECK(zl.lt == 0);

    ParsedReport upper{};
    auto upperReport = switchReport(0x00, 0x38);
    REQUIRE(decodeFromLayout(upperReport.data(), upperReport.size(), upper, L));
    CHECK(upper.wButtons == 0);
}

TEST_CASE("switch-order: face buttons remap by position", "[usb-hid-layout][switch-order]") {
    HidLayout L = switchLayout(true);
    struct Case {
        std::uint8_t bit;
        std::uint16_t expected;
    };
    const Case cases[] = {
        {0x01, layout::kXusbX}, // Y (west)
        {0x02, layout::kXusbA}, // B (south)
        {0x04, layout::kXusbB}, // A (east)
        {0x08, layout::kXusbY}, // X (north)
    };
    for (const Case& c : cases) {
        ParsedReport s{};
        auto r = switchReport(c.bit, 0x00);
        REQUIRE(decodeFromLayout(r.data(), r.size(), s, L));
        CHECK(s.wButtons == c.expected);
    }
}

TEST_CASE("switch-order: bumpers map and ZL ZR drive the triggers",
          "[usb-hid-layout][switch-order]") {
    HidLayout L = switchLayout(true);

    ParsedReport bumpers{};
    auto lr = switchReport(0x30, 0x00);
    REQUIRE(decodeFromLayout(lr.data(), lr.size(), bumpers, L));
    CHECK(bumpers.wButtons ==
          static_cast<std::uint16_t>(layout::kXusbLeftShoulder | layout::kXusbRightShoulder));
    CHECK(bumpers.lt == 0);
    CHECK(bumpers.rt == 0);

    ParsedReport triggers{};
    auto zlzr = switchReport(0xC0, 0x00);
    REQUIRE(decodeFromLayout(zlzr.data(), zlzr.size(), triggers, L));
    CHECK(triggers.wButtons == 0);
    CHECK(triggers.lt == 255);
    CHECK(triggers.rt == 255);

    auto released = switchReport(0x00, 0x00);
    REQUIRE(decodeFromLayout(released.data(), released.size(), triggers, L));
    CHECK(triggers.lt == 0);
    CHECK(triggers.rt == 0);
}

TEST_CASE("switch-order: the upper row maps minus plus sticks and home",
          "[usb-hid-layout][switch-order]") {
    HidLayout L = switchLayout(true);
    struct Case {
        std::uint8_t bit;
        std::uint16_t expected;
    };
    const Case cases[] = {
        {0x01, layout::kXusbBack},       // Minus
        {0x02, layout::kXusbStart},      // Plus
        {0x04, layout::kXusbLeftThumb},  // L3
        {0x08, layout::kXusbRightThumb}, // R3
        {0x10, layout::kXusbGuide},      // Home
        {0x20, 0},                       // Capture: no XUSB equivalent
    };
    for (const Case& c : cases) {
        ParsedReport s{};
        auto r = switchReport(0x00, c.bit);
        REQUIRE(decodeFromLayout(r.data(), r.size(), s, L));
        CHECK(s.wButtons == c.expected);
    }
}

TEST_CASE("switch-order: hat and sticks are untouched by the remap",
          "[usb-hid-layout][switch-order]") {
    HidLayout L = switchLayout(true);

    ParsedReport east{};
    auto r = switchReport(0x00, 0x00, 0x02, 0xFF);
    REQUIRE(decodeFromLayout(r.data(), r.size(), east, L));
    CHECK((east.wButtons & layout::kXusbDpadRight) != 0);
    CHECK(east.lx > 30000);

    ParsedReport neutral{};
    auto n = switchReport(0x00, 0x00);
    REQUIRE(decodeFromLayout(n.data(), n.size(), neutral, L));
    CHECK((neutral.wButtons & kDpadMask) == 0);
    CHECK(neutral.lx == 0);
}

TEST_CASE("switch-order: a combined report decodes all fields", "[usb-hid-layout][switch-order]") {
    HidLayout L = switchLayout(true);
    ParsedReport s{};
    auto r = switchReport(0x44, 0x02, 0x04, 0xFF);
    REQUIRE(decodeFromLayout(r.data(), r.size(), s, L));
    CHECK(s.wButtons ==
          static_cast<std::uint16_t>(layout::kXusbB | layout::kXusbStart | layout::kXusbDpadDown));
    CHECK(s.lt == 255);
    CHECK(s.rt == 0);
    CHECK(s.lx > 30000);
}
