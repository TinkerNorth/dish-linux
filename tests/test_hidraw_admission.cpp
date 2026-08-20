// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// HidrawGateway's admission gate — the second, independent report-descriptor
// walker in this codebase. core/input/UsbHidLayout's parseReportDescriptor is
// pinned byte for byte by test_usb_hid_layout.cpp; this one runs first and
// decides whether a device is opened at all, on bytes that came straight off an
// untrusted USB device, so its bounds arithmetic is the part worth proving.
//
// The gamepad fixtures are the ones from test_usb_hid_layout.cpp, copied rather
// than shared because they live in that file's anonymous namespace. Running the
// same bytes through both walkers is the point: a descriptor the layout parser
// decodes must also be one this gate admits, or the device is dropped before
// the parser ever sees it.
//
// The malformed descriptors are exact-sized heap buffers so a single byte of
// overread lands in an ASan redzone instead of in whatever followed on the
// stack. The suite's address+undefined leg is what turns those cases from an
// assertion about a return value into a check of the walk itself.

#include "source/usb/HidrawGateway.h"

#include "core/input/UsbReportParsers.h"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

using dish::input::usbparse::HidParser;
using dish::source::usb::detail::collectionMatchesParser;
using dish::source::usb::detail::topLevelUsage;

namespace {

// HID Usage Table values, spelled out so the test pins the spec rather than
// re-reading the constants under test.
constexpr std::uint16_t kGenericDesktop = 0x01;
constexpr std::uint16_t kConsumer = 0x0C;
constexpr std::uint16_t kVendorPage = 0xFF00;
constexpr std::uint16_t kPointer = 0x01;
constexpr std::uint16_t kMouse = 0x02;
constexpr std::uint16_t kJoystick = 0x04;
constexpr std::uint16_t kGamepad = 0x05;
constexpr std::uint16_t kKeyboard = 0x06;

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

// A single 32-bit X axis with a 31-bit logical max: the 4-byte item form, which
// is the one the size-3-means-4 rule decides.
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

// PDP Faceoff Wired Pro (0e6f:0180): 14 buttons in Switch usage order, then the
// hat and X/Y/Z/Rz. Its usage page moves to Button inside the collection, which
// is past where this walker stops.
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

// Vendor page, one byte of input: the shape of the Steam Controller's game
// interface, and the descriptor the layout parser rejects.
const std::uint8_t kVendorDescriptor[] = {0x06, 0x00, 0xFF, 0x09, 0x01, 0xA1, 0x01,
                                          0x75, 0x08, 0x95, 0x01, 0x81, 0x02, 0xC0};

struct Fixture {
    const char* what;
    const std::uint8_t* bytes;
    std::size_t len;
};

const Fixture kGamepadFixtures[] = {
    {"standard two-stick gamepad", kGamepadDescriptor, sizeof(kGamepadDescriptor)},
    {"gamepad behind a report id", kReportIdDescriptor, sizeof(kReportIdDescriptor)},
    {"wide-axis gamepad", kWideAxisDescriptor, sizeof(kWideAxisDescriptor)},
    {"narrow-hat gamepad", kNarrowHatDescriptor, sizeof(kNarrowHatDescriptor)},
    {"PDP Faceoff switch-order gamepad", kSwitchOrderDescriptor, sizeof(kSwitchOrderDescriptor)},
};

// Every parser but the Steam Controller's shares one admission rule.
constexpr HidParser kShapeAdmittedParsers[] = {HidParser::DualShock4, HidParser::DualSense,
                                               HidParser::SwitchProUsb, HidParser::GenericHid};

} // namespace

TEST_CASE("hidraw admission reads the top-level usage off every gamepad fixture",
          "[hidraw-admission]") {
    for (const Fixture& fixture : kGamepadFixtures) {
        INFO(fixture.what);
        std::uint16_t page = 0;
        std::uint16_t usage = 0;
        REQUIRE(topLevelUsage(fixture.bytes, fixture.len, page, usage));
        CHECK(page == kGenericDesktop);
        CHECK((usage == kGamepad || usage == kJoystick));
    }
}

TEST_CASE("hidraw admission reads a vendor-defined top-level usage", "[hidraw-admission]") {
    std::uint16_t page = 0;
    std::uint16_t usage = 0;
    REQUIRE(topLevelUsage(kVendorDescriptor, sizeof(kVendorDescriptor), page, usage));
    CHECK(page == kVendorPage);
}

TEST_CASE("hidraw admission refuses a malformed descriptor without reading past its end",
          "[hidraw-admission]") {
    struct Case {
        const char* what;
        std::vector<std::uint8_t> bytes;
    };
    const Case cases[] = {
        {"long item", {0xFE, 0x02, 0x01, 0xAA, 0xBB}},
        {"truncated long item", {0xFE}},
        {"single truncated item", {0x26}},
        {"collection prefix with its data byte missing", {0x05, 0x01, 0x09, 0x05, 0xA1}},
        {"usage page promising two bytes and supplying one", {0x06, 0x00}},
        {"four-byte item supplying two", {0x07, 0x01, 0x00}},
        {"usage with no collection at all", {0x05, 0x01, 0x09, 0x05}},
        {"usage with a physical collection only", {0x05, 0x01, 0x09, 0x05, 0xA1, 0x00, 0xC0}},
    };

    for (const Case& c : cases) {
        INFO(c.what);
        std::uint16_t page = 0;
        std::uint16_t usage = 0;
        CHECK_FALSE(topLevelUsage(c.bytes.data(), c.bytes.size(), page, usage));
    }
}

TEST_CASE("hidraw admission refuses an empty descriptor", "[hidraw-admission]") {
    std::uint16_t page = 0;
    std::uint16_t usage = 0;
    CHECK_FALSE(topLevelUsage(nullptr, 0, page, usage));
}

TEST_CASE("hidraw admission admits the Steam Controller only on its vendor page",
          "[hidraw-admission]") {
    CHECK(collectionMatchesParser(kVendorPage, 0x01, HidParser::SteamController));

    // The pad also publishes a keyboard and a mouse collection; claiming either
    // would reconfigure the wrong interface. A gamepad collection is refused
    // too — the game interface declares no gamepad usage, so one that does is
    // some other device sharing the vid:pid.
    CHECK_FALSE(collectionMatchesParser(kGenericDesktop, kKeyboard, HidParser::SteamController));
    CHECK_FALSE(collectionMatchesParser(kGenericDesktop, kMouse, HidParser::SteamController));
    CHECK_FALSE(collectionMatchesParser(kGenericDesktop, kGamepad, HidParser::SteamController));
}

TEST_CASE("hidraw admission requires a gamepad or joystick collection of every other parser",
          "[hidraw-admission]") {
    for (const HidParser parser : kShapeAdmittedParsers) {
        CHECK(collectionMatchesParser(kGenericDesktop, kGamepad, parser));
        CHECK(collectionMatchesParser(kGenericDesktop, kJoystick, parser));

        CHECK_FALSE(collectionMatchesParser(kGenericDesktop, kKeyboard, parser));
        CHECK_FALSE(collectionMatchesParser(kGenericDesktop, kMouse, parser));
        CHECK_FALSE(collectionMatchesParser(kGenericDesktop, kPointer, parser));
        CHECK_FALSE(collectionMatchesParser(kConsumer, kGamepad, parser));
        CHECK_FALSE(collectionMatchesParser(kVendorPage, kGamepad, parser));
    }
}

TEST_CASE("hidraw admission walks a real descriptor into a claim decision", "[hidraw-admission]") {
    // The two halves as enumerate() runs them: descriptor in, verdict out.
    for (const Fixture& fixture : kGamepadFixtures) {
        INFO(fixture.what);
        std::uint16_t page = 0;
        std::uint16_t usage = 0;
        REQUIRE(topLevelUsage(fixture.bytes, fixture.len, page, usage));
        CHECK(collectionMatchesParser(page, usage, HidParser::GenericHid));
        CHECK_FALSE(collectionMatchesParser(page, usage, HidParser::SteamController));
    }

    std::uint16_t page = 0;
    std::uint16_t usage = 0;
    REQUIRE(topLevelUsage(kVendorDescriptor, sizeof(kVendorDescriptor), page, usage));
    CHECK(collectionMatchesParser(page, usage, HidParser::SteamController));
    CHECK_FALSE(collectionMatchesParser(page, usage, HidParser::GenericHid));
}
