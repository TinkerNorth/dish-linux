// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The descriptors below are real devices' bytes: the 8BitDo M30 over USB in its D-input mode as
// fwupd's plugins/ebitdo/data/m30.txt lists it item by item, the 8BitDo USB Wireless Adapter 2 in
// D-input mode as sys-con issue 65's log carries it with its captured reports, and the PlayStation
// Classic controller as J2Kbr/TitanTwo issue 177 dumped it. The M30's own input reports were
// never captured; they are built from its descriptor.

#include "core/input/UsbHidLayout.h"

#include "core/input/UsbReportParsers.h"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

using dish::input::usbhid::ButtonOrder;
using dish::input::usbhid::decodeFromLayout;
using dish::input::usbhid::HidLayout;
using dish::input::usbhid::parseReportDescriptor;
using dish::input::usbparse::buttonOrderForDevice;
using dish::input::usbparse::HidParser;
using dish::input::usbparse::lookupKnownModel;
using dish::input::usbparse::ParsedReport;
using dish::input::usbparse::parserForDevice;
namespace layout = dish::input::layout;

namespace {

constexpr std::uint16_t kDpadMask =
    layout::kXusbDpadUp | layout::kXusbDpadDown | layout::kXusbDpadLeft | layout::kXusbDpadRight;

// 8BitDo M30, USB D-input, 2dc8:5006. 15 buttons, a hat, X/Y/Z/Rz, Accelerator and Brake, in a
// 9-byte report with no report id.
const std::uint8_t kM30UsbDescriptor[] = {
    0x05, 0x01, 0x09, 0x05, 0xa1, 0x01, 0x15, 0x00, 0x25, 0x01, 0x35, 0x00, 0x45, 0x01, 0x75, 0x01,
    0x95, 0x0f, 0x05, 0x09, 0x19, 0x01, 0x29, 0x0f, 0x81, 0x02, 0x95, 0x01, 0x81, 0x01, 0x05, 0x01,
    0x25, 0x07, 0x46, 0x3b, 0x01, 0x75, 0x04, 0x95, 0x01, 0x65, 0x14, 0x09, 0x39, 0x81, 0x42, 0x65,
    0x00, 0x95, 0x01, 0x81, 0x01, 0x26, 0xff, 0x00, 0x46, 0xff, 0x00, 0x09, 0x30, 0x09, 0x31, 0x09,
    0x32, 0x09, 0x35, 0x75, 0x08, 0x95, 0x04, 0x81, 0x02, 0x05, 0x02, 0x15, 0x00, 0x26, 0xff, 0x00,
    0x09, 0xc4, 0x09, 0xc5, 0x95, 0x02, 0x75, 0x08, 0x81, 0x02, 0x05, 0x08, 0x09, 0x43, 0x15, 0x00,
    0x26, 0xff, 0x00, 0x35, 0x00, 0x46, 0xff, 0x00, 0x75, 0x08, 0x95, 0x02, 0x91, 0x82, 0x09, 0x44,
    0x91, 0x82, 0x09, 0x45, 0x91, 0x82, 0x09, 0x46, 0x91, 0x82, 0xc0,
};

// 8BitDo USB Wireless Adapter 2, D-input, 2dc8:3105. The M30's layout behind report id 1, plus a
// vendor input report 2 and an output report 0x81 the decode never touches.
const std::uint8_t kAdapter2Descriptor[] = {
    0x05, 0x01, 0x09, 0x05, 0xA1, 0x01, 0x85, 0x01, 0x15, 0x00, 0x25, 0x01, 0x35, 0x00, 0x45,
    0x01, 0x75, 0x01, 0x95, 0x0F, 0x05, 0x09, 0x19, 0x01, 0x29, 0x0F, 0x81, 0x02, 0x95, 0x01,
    0x81, 0x01, 0x05, 0x01, 0x25, 0x07, 0x46, 0x3B, 0x01, 0x75, 0x04, 0x95, 0x01, 0x65, 0x14,
    0x09, 0x39, 0x81, 0x42, 0x65, 0x00, 0x95, 0x01, 0x81, 0x01, 0x26, 0xFF, 0x00, 0x46, 0xFF,
    0x00, 0x09, 0x30, 0x09, 0x31, 0x09, 0x32, 0x09, 0x35, 0x75, 0x08, 0x95, 0x04, 0x81, 0x02,
    0x05, 0x02, 0x15, 0x00, 0x26, 0xFF, 0x00, 0x09, 0xC4, 0x09, 0xC5, 0x95, 0x02, 0x75, 0x08,
    0x81, 0x02, 0x05, 0x08, 0x09, 0x43, 0x15, 0x00, 0x26, 0xFF, 0x00, 0x35, 0x00, 0x46, 0xFF,
    0x00, 0x75, 0x08, 0x95, 0x02, 0x91, 0x82, 0x09, 0x44, 0x91, 0x82, 0x09, 0x45, 0x91, 0x82,
    0x09, 0x46, 0x91, 0x82, 0x85, 0x02, 0x09, 0x02, 0x75, 0x08, 0x95, 0x3F, 0x81, 0x03, 0x85,
    0x81, 0x09, 0x03, 0x75, 0x08, 0x95, 0x3F, 0x91, 0x83, 0xC0,
};

// PlayStation Classic controller, 054c:0cda. Ten buttons, then X and Y as two-bit, three-state
// directions, in a 2-byte report with no report id.
const std::uint8_t kPsClassicDescriptor[] = {
    0x05, 0x01, 0x09, 0x05, 0xA1, 0x01, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95,
    0x0A, 0x05, 0x09, 0x19, 0x01, 0x29, 0x0A, 0x81, 0x02, 0x05, 0x01, 0x09, 0x30,
    0x09, 0x31, 0x15, 0x00, 0x25, 0x02, 0x35, 0x00, 0x45, 0x02, 0x75, 0x02, 0x95,
    0x02, 0x81, 0x02, 0x75, 0x01, 0x95, 0x02, 0x81, 0x01, 0xC0,
};

HidLayout parsed(const std::uint8_t* desc, std::size_t len, ButtonOrder order) {
    HidLayout L;
    REQUIRE(parseReportDescriptor(desc, len, L));
    L.order = order;
    return L;
}

ParsedReport decode(const HidLayout& L, const std::vector<std::uint8_t>& report) {
    ParsedReport s{};
    REQUIRE(decodeFromLayout(report.data(), report.size(), s, L));
    return s;
}

// M30 USB report: buttons 1-8 in byte 0, 9-15 in byte 1, the hat in byte 2's low nibble (0x0F
// when centred), X Y Z Rz at 0x7F centre, Accelerator and Brake last.
std::vector<std::uint8_t> m30Report(std::uint8_t buttonsLow, std::uint8_t buttonsHigh,
                                    std::uint8_t hat = 0x0F) {
    return {buttonsLow, buttonsHigh, hat, 0x7F, 0x7F, 0x7F, 0x7F, 0x00, 0x00};
}

} // namespace

TEST_CASE("the 8BitDo D-input identities decode generic HID in the 8BitDo button order",
          "[usb-hid-layout][8bitdo]") {
    for (const int pid : {0x5006, 0x3100, 0x3105}) {
        CHECK(parserForDevice(0x2DC8, pid) == HidParser::GenericHid);
        CHECK(buttonOrderForDevice(0x2DC8, pid) == ButtonOrder::EightBitDo);
        CHECK(lookupKnownModel(0x2DC8, pid) != nullptr);
    }
    CHECK(buttonOrderForDevice(0x2DC8, 0x9015) == ButtonOrder::Western);
}

TEST_CASE("the M30 descriptor parses to its field map", "[usb-hid-layout][8bitdo]") {
    CHECK(sizeof(kM30UsbDescriptor) == 123);
    const HidLayout L =
        parsed(kM30UsbDescriptor, sizeof(kM30UsbDescriptor), ButtonOrder::EightBitDo);
    CHECK(L.reportId == 0);
    CHECK(L.buttonCount == 15);
    CHECK(L.buttonBitOffset == 0);
    CHECK(L.hasHat);
    CHECK(L.hatBitOffset == 16);
    CHECK(L.lx.present);
    CHECK(L.lx.bitOffset == 24);
    CHECK(L.ly.bitOffset == 32);
    CHECK(L.rx.bitOffset == 40);
    CHECK(L.ry.bitOffset == 48);
}

TEST_CASE("M30 face buttons keep their letters and Heart is Guide", "[usb-hid-layout][8bitdo]") {
    const HidLayout L =
        parsed(kM30UsbDescriptor, sizeof(kM30UsbDescriptor), ButtonOrder::EightBitDo);
    CHECK(decode(L, m30Report(0x01, 0x00)).wButtons == layout::kXusbA);
    CHECK(decode(L, m30Report(0x02, 0x00)).wButtons == layout::kXusbB);
    CHECK(decode(L, m30Report(0x04, 0x00)).wButtons == layout::kXusbGuide); // Heart, firmware 1.x
    CHECK(decode(L, m30Report(0x08, 0x00)).wButtons == layout::kXusbX);
    CHECK(decode(L, m30Report(0x10, 0x00)).wButtons == layout::kXusbY);
    CHECK(decode(L, m30Report(0x20, 0x00)).wButtons == 0); // button 6 is not on the pad
    CHECK(decode(L, m30Report(0x00, 0x10)).wButtons == layout::kXusbGuide); // Heart, firmware 2.00
}

TEST_CASE("M30 Z and C are the shoulders, Select is Back and Start is Start",
          "[usb-hid-layout][8bitdo]") {
    const HidLayout L =
        parsed(kM30UsbDescriptor, sizeof(kM30UsbDescriptor), ButtonOrder::EightBitDo);
    CHECK(decode(L, m30Report(0x40, 0x00)).wButtons == layout::kXusbLeftShoulder);
    CHECK(decode(L, m30Report(0x80, 0x00)).wButtons == layout::kXusbRightShoulder);
    CHECK(decode(L, m30Report(0x00, 0x04)).wButtons == layout::kXusbBack);
    CHECK(decode(L, m30Report(0x00, 0x08)).wButtons == layout::kXusbStart);
    CHECK(decode(L, m30Report(0x00, 0x20)).wButtons == layout::kXusbLeftThumb);
    CHECK(decode(L, m30Report(0x00, 0x40)).wButtons == layout::kXusbRightThumb);
}

TEST_CASE("M30 L and R each drive one trigger and nothing else", "[usb-hid-layout][8bitdo]") {
    const HidLayout L =
        parsed(kM30UsbDescriptor, sizeof(kM30UsbDescriptor), ButtonOrder::EightBitDo);
    const ParsedReport left = decode(L, m30Report(0x00, 0x01));
    CHECK(left.lt == 255);
    CHECK(left.rt == 0);
    CHECK(left.wButtons == 0);
    const ParsedReport right = decode(L, m30Report(0x00, 0x02));
    CHECK(right.lt == 0);
    CHECK(right.rt == 255);
    CHECK(right.wButtons == 0);
}

TEST_CASE("M30 Accelerator and Brake bytes are not read as triggers in the 8BitDo order",
          "[usb-hid-layout][8bitdo]") {
    // The pad's L and R are digital; the two analog usages only ever echo them.
    const HidLayout L =
        parsed(kM30UsbDescriptor, sizeof(kM30UsbDescriptor), ButtonOrder::EightBitDo);
    const std::vector<std::uint8_t> analogOnly{0x00, 0x00, 0x0F, 0x7F, 0x7F,
                                               0x7F, 0x7F, 0xFF, 0xFF};
    const ParsedReport s = decode(L, analogOnly);
    CHECK(s.lt == 0);
    CHECK(s.rt == 0);
}

TEST_CASE("M30 d-pad arrives as the hat in d-pad mode and as the left stick in stick mode",
          "[usb-hid-layout][8bitdo]") {
    const HidLayout L =
        parsed(kM30UsbDescriptor, sizeof(kM30UsbDescriptor), ButtonOrder::EightBitDo);
    CHECK((decode(L, m30Report(0x00, 0x00, 0x00)).wButtons & kDpadMask) == layout::kXusbDpadUp);
    CHECK((decode(L, m30Report(0x00, 0x00, 0x06)).wButtons & kDpadMask) == layout::kXusbDpadLeft);
    const std::vector<std::uint8_t> stickLeft{0x00, 0x00, 0x0F, 0x00, 0x7F, 0x7F, 0x7F, 0x00, 0x00};
    const ParsedReport s = decode(L, stickLeft);
    CHECK(s.lx < -32000);
    CHECK((s.wButtons & kDpadMask) == 0);
}

TEST_CASE("the western order scrambles the M30, which is what the 8BitDo order exists to fix",
          "[usb-hid-layout][8bitdo]") {
    const HidLayout western =
        parsed(kM30UsbDescriptor, sizeof(kM30UsbDescriptor), ButtonOrder::Western);
    CHECK(decode(western, m30Report(0x00, 0x08)).wButtons == 0);              // Start is lost
    CHECK(decode(western, m30Report(0x04, 0x00)).wButtons == layout::kXusbX); // Heart lands on X
}

TEST_CASE("the Adapter 2 descriptor locks report 1 and decodes its captured reports",
          "[usb-hid-layout][8bitdo]") {
    CHECK(sizeof(kAdapter2Descriptor) == 145);
    const HidLayout L =
        parsed(kAdapter2Descriptor, sizeof(kAdapter2Descriptor), ButtonOrder::EightBitDo);
    CHECK(L.reportId == 1);
    CHECK(L.buttonCount == 15);
    CHECK(L.hatBitOffset == 16);

    const ParsedReport idle =
        decode(L, {0x01, 0x00, 0x00, 0x0F, 0x7C, 0x82, 0x7F, 0x82, 0x00, 0x00});
    CHECK(idle.wButtons == 0);
    CHECK(idle.lt == 0);
    CHECK(idle.rt == 0);
    CHECK(idle.lx > -2000);
    CHECK(idle.lx < 0);

    const ParsedReport viewLeft =
        decode(L, {0x01, 0x00, 0x04, 0x06, 0x7C, 0x82, 0x7F, 0x81, 0x00, 0x00});
    CHECK(viewLeft.wButtons == (layout::kXusbBack | layout::kXusbDpadLeft));

    const ParsedReport right =
        decode(L, {0x01, 0x00, 0x00, 0x02, 0x7C, 0x82, 0x7F, 0x82, 0x00, 0x00});
    CHECK(right.wButtons == layout::kXusbDpadRight);

    ParsedReport wrongId{};
    const std::vector<std::uint8_t> vendor{0x02, 0x00, 0x00, 0x0F, 0x7C,
                                           0x82, 0x7F, 0x82, 0x00, 0x00};
    CHECK_FALSE(decodeFromLayout(vendor.data(), vendor.size(), wrongId, L));
}

TEST_CASE("only the DualShock 4 identities decode as a DualShock 4", "[usb-parsers][sony]") {
    CHECK(parserForDevice(0x054C, 0x05C4) == HidParser::DualShock4);
    CHECK(parserForDevice(0x054C, 0x09CC) == HidParser::DualShock4);
    CHECK(parserForDevice(0x054C, 0x0BA0) == HidParser::DualShock4); // the USB wireless adaptor
    CHECK(parserForDevice(0x054C, 0x0CE6) == HidParser::DualSense);
    CHECK(parserForDevice(0x054C, 0x0DF2) == HidParser::DualSense);
    CHECK(parserForDevice(0x054C, 0x0CDA) == HidParser::GenericHid); // PlayStation Classic
    CHECK(parserForDevice(0x054C, 0x0268) == HidParser::GenericHid); // a Sixaxis is no DualShock 4
}

TEST_CASE("the PlayStation Classic controller is a known model in its own button order",
          "[usb-hid-layout][psclassic]") {
    CHECK(buttonOrderForDevice(0x054C, 0x0CDA) == ButtonOrder::PlayStationClassic);
    REQUIRE(lookupKnownModel(0x054C, 0x0CDA) != nullptr);
}

TEST_CASE("the PlayStation Classic descriptor parses to ten buttons and two three-state axes",
          "[usb-hid-layout][psclassic]") {
    CHECK(sizeof(kPsClassicDescriptor) == 49);
    const HidLayout L =
        parsed(kPsClassicDescriptor, sizeof(kPsClassicDescriptor), ButtonOrder::PlayStationClassic);
    CHECK(L.reportId == 0);
    CHECK(L.buttonCount == 10);
    CHECK(L.lx.present);
    CHECK(L.lx.bitOffset == 10);
    CHECK(L.lx.bitSize == 2);
    CHECK(L.lx.logicalMax == 2);
    CHECK(L.ly.bitOffset == 12);
    CHECK_FALSE(L.hasHat);
}

TEST_CASE("PlayStation Classic buttons map by position", "[usb-hid-layout][psclassic]") {
    const HidLayout L =
        parsed(kPsClassicDescriptor, sizeof(kPsClassicDescriptor), ButtonOrder::PlayStationClassic);
    // X and Y centred (value 1 in bits 2-3 and 4-5 of byte 1) is 0x14.
    CHECK(decode(L, {0x01, 0x14}).wButtons == layout::kXusbY);             // Triangle
    CHECK(decode(L, {0x02, 0x14}).wButtons == layout::kXusbB);             // Circle
    CHECK(decode(L, {0x04, 0x14}).wButtons == layout::kXusbA);             // Cross
    CHECK(decode(L, {0x08, 0x14}).wButtons == layout::kXusbX);             // Square
    CHECK(decode(L, {0x40, 0x14}).wButtons == layout::kXusbLeftShoulder);  // L1
    CHECK(decode(L, {0x80, 0x14}).wButtons == layout::kXusbRightShoulder); // R1
    CHECK(decode(L, {0x00, 0x15}).wButtons == layout::kXusbBack);          // Select
    CHECK(decode(L, {0x00, 0x16}).wButtons == layout::kXusbStart);         // Start
    const ParsedReport l2 = decode(L, {0x10, 0x14});
    CHECK(l2.lt == 255);
    CHECK(l2.wButtons == 0);
    const ParsedReport r2 = decode(L, {0x20, 0x14});
    CHECK(r2.rt == 255);
    CHECK(r2.wButtons == 0);
}

TEST_CASE("a three-state axis is a direction pad, not a stick", "[usb-hid-layout][psclassic]") {
    const HidLayout L =
        parsed(kPsClassicDescriptor, sizeof(kPsClassicDescriptor), ButtonOrder::PlayStationClassic);
    const ParsedReport centred = decode(L, {0x00, 0x14});
    CHECK((centred.wButtons & kDpadMask) == 0);
    CHECK(centred.lx == 0);
    CHECK(centred.ly == 0);
    CHECK(decode(L, {0x00, 0x10}).wButtons == layout::kXusbDpadLeft);  // X = 0
    CHECK(decode(L, {0x00, 0x18}).wButtons == layout::kXusbDpadRight); // X = 2
    CHECK(decode(L, {0x00, 0x04}).wButtons == layout::kXusbDpadUp);    // Y = 0
    CHECK(decode(L, {0x00, 0x24}).wButtons == layout::kXusbDpadDown);  // Y = 2
    CHECK(decode(L, {0x00, 0x20}).wButtons == (layout::kXusbDpadDown | layout::kXusbDpadLeft));
    CHECK(decode(L, {0x00, 0x18}).lx == 0);
}

TEST_CASE("a one-byte logical maximum of 0xFF over a non-negative minimum reads as 255",
          "[usb-hid-layout][logical-max]") {
    // Linux hid-core reads the maximum unsigned whenever the minimum is not negative; a parse that
    // sign-extends it to -1 leaves the axis with no range at all.
    const std::uint8_t descriptor[] = {
        0x05, 0x01, 0x09, 0x05, 0xA1, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15, 0x00, 0x25, 0xFF, 0x75,
        0x08, 0x95, 0x02, 0x81, 0x02, 0x05, 0x09, 0x19, 0x01, 0x29, 0x02, 0x15, 0x00, 0x25, 0x01,
        0x75, 0x01, 0x95, 0x02, 0x81, 0x02, 0x75, 0x06, 0x95, 0x01, 0x81, 0x01, 0xC0,
    };
    const HidLayout L = parsed(descriptor, sizeof(descriptor), ButtonOrder::Western);
    CHECK(L.lx.logicalMax == 255);
    CHECK(decode(L, {0xFF, 0x7F, 0x00}).lx == 32767);
    CHECK(decode(L, {0x00, 0x7F, 0x00}).lx == -32767);
    CHECK(decode(L, {0x7F, 0x7F, 0x00}).lx == 0);
}

TEST_CASE("a one-byte logical maximum over a negative minimum keeps its sign",
          "[usb-hid-layout][logical-max]") {
    const std::uint8_t descriptor[] = {
        0x05, 0x01, 0x09, 0x05, 0xA1, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15, 0x81, 0x25, 0x7F, 0x75,
        0x08, 0x95, 0x02, 0x81, 0x02, 0x05, 0x09, 0x19, 0x01, 0x29, 0x02, 0x15, 0x00, 0x25, 0x01,
        0x75, 0x01, 0x95, 0x02, 0x81, 0x02, 0x75, 0x06, 0x95, 0x01, 0x81, 0x01, 0xC0,
    };
    const HidLayout L = parsed(descriptor, sizeof(descriptor), ButtonOrder::Western);
    CHECK(L.lx.logicalMin == -127);
    CHECK(L.lx.logicalMax == 127);
    CHECK(decode(L, {0x7F, 0x00, 0x00}).lx == 32767);
    CHECK(decode(L, {0x81, 0x00, 0x00}).lx == -32767);
}

TEST_CASE("a full-range signed 32-bit axis scales without overflowing",
          "[usb-hid-layout][wide-axis]") {
    const std::uint8_t descriptor[] = {
        0x05, 0x01, 0x09, 0x05, 0xA1, 0x01, 0x09, 0x30, 0x09, 0x31, 0x17, 0x00, 0x00, 0x00, 0x80,
        0x27, 0xFF, 0xFF, 0xFF, 0x7F, 0x75, 0x20, 0x95, 0x02, 0x81, 0x02, 0x05, 0x09, 0x19, 0x01,
        0x29, 0x08, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0xC0,
    };
    const HidLayout L = parsed(descriptor, sizeof(descriptor), ButtonOrder::Western);
    REQUIRE(L.lx.bitSize == 32);
    CHECK(decode(L, {0xFF, 0xFF, 0xFF, 0x7F, 0x00, 0x00, 0x00, 0x00, 0x00}).lx == 32767);
    CHECK(decode(L, {0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00}).lx == -32767);
    CHECK(decode(L, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}).lx == 0);
}

TEST_CASE("a full-range signed 32-bit trigger scales without overflowing",
          "[usb-hid-layout][wide-axis]") {
    const std::uint8_t descriptor[] = {
        0x05, 0x01, 0x09, 0x05, 0xA1, 0x01, 0x09, 0x33, 0x09, 0x34, 0x17, 0x00, 0x00, 0x00, 0x80,
        0x27, 0xFF, 0xFF, 0xFF, 0x7F, 0x75, 0x20, 0x95, 0x02, 0x81, 0x02, 0x05, 0x09, 0x19, 0x01,
        0x29, 0x08, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0xC0,
    };
    const HidLayout L = parsed(descriptor, sizeof(descriptor), ButtonOrder::Western);
    REQUIRE(L.lt.bitSize == 32);
    CHECK(decode(L, {0xFF, 0xFF, 0xFF, 0x7F, 0x00, 0x00, 0x00, 0x00, 0x00}).lt == 255);
    CHECK(decode(L, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}).lt == 127);
    CHECK(decode(L, {0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00}).lt == 0);
}
