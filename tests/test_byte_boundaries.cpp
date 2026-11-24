// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The byte-level assembly every parser and codec rests on, at the values a wrong shift, mask or
// sign extension gets wrong: 0x00, 0x7F, 0x80 and 0xFF in each position that is shifted, the sign
// bit of a 16-bit word, the top bit of a 32-bit one. These pin behaviour, so they hold before and
// after the parsers move to unsigned-only bitwise arithmetic.

#include "Util/Endian.h"
#include "core/input/GamepadButtonLayouts.h"
#include "core/input/UsbHidLayout.h"
#include "core/input/UsbReportParsers.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <vector>

namespace usbparse = dish::input::usbparse;
namespace usbhid = dish::input::usbhid;
namespace layout = dish::input::layout;
namespace util = dish::util;

TEST_CASE("a little-endian int16 takes its sign from the high byte alone", "[bytes][usb-parsers]") {
    const std::array<std::uint8_t, 2> zero{0x00, 0x00};
    const std::array<std::uint8_t, 2> lowTop{0x80, 0x00};
    const std::array<std::uint8_t, 2> highTop{0x00, 0x80};
    const std::array<std::uint8_t, 2> max{0xFF, 0x7F};
    const std::array<std::uint8_t, 2> minusOne{0xFF, 0xFF};
    const std::array<std::uint8_t, 2> mixed{0x7F, 0x80};
    CHECK(usbparse::rdLe16(zero.data(), 0) == 0);
    CHECK(usbparse::rdLe16(lowTop.data(), 0) == 128);
    CHECK(usbparse::rdLe16(highTop.data(), 0) == -32768);
    CHECK(usbparse::rdLe16(max.data(), 0) == 32767);
    CHECK(usbparse::rdLe16(minusOne.data(), 0) == -1);
    CHECK(usbparse::rdLe16(mixed.data(), 0) == -32641);
}

TEST_CASE("a DualShock 4 stick byte maps 0x00, 0x7F, 0x80 and 0xFF to the int16 span",
          "[bytes][usb-parsers][ds4]") {
    std::vector<std::uint8_t> r(10, 0);
    r[0] = 0x01;
    r[1] = 0x00; // LX hard left
    r[2] = 0x00; // LY hard up (inverted on the wire)
    r[3] = 0xFF; // RX hard right
    r[4] = 0xFF; // RY hard down
    r[5] = 0x08; // hat neutral
    usbparse::ParsedReport out;
    usbparse::StickAutoRangeState sticks;
    REQUIRE(
        usbparse::decodeReport(usbparse::HidParser::DualShock4, r.data(), r.size(), out, sticks));
    CHECK(out.lx == -32768);
    CHECK(out.ly == 32767);
    CHECK(out.rx == 32639);
    CHECK(out.ry == -32639);

    r[1] = 0x7F;
    r[2] = 0x80;
    r[3] = 0x80;
    r[4] = 0x7F;
    REQUIRE(
        usbparse::decodeReport(usbparse::HidParser::DualShock4, r.data(), r.size(), out, sticks));
    CHECK(out.lx == -257);
    CHECK(out.ly == 0);
    CHECK(out.rx == 0);
    CHECK(out.ry == 257);
}

TEST_CASE("every face button a DualShock 4 byte carries survives its top bit",
          "[bytes][usb-parsers][ds4]") {
    std::vector<std::uint8_t> r(10, 0x80);
    r[0] = 0x01;
    r[5] = 0xF8; // all four face buttons, hat neutral
    r[6] = 0xFF; // shoulders, triggers' digital bits, Share, Options, both thumbs
    r[7] = 0x01; // PS
    usbparse::ParsedReport out;
    usbparse::StickAutoRangeState sticks;
    REQUIRE(
        usbparse::decodeReport(usbparse::HidParser::DualShock4, r.data(), r.size(), out, sticks));
    const int expected = layout::kXusbA | layout::kXusbB | layout::kXusbX | layout::kXusbY |
                         layout::kXusbLeftShoulder | layout::kXusbRightShoulder |
                         layout::kXusbBack | layout::kXusbStart | layout::kXusbLeftThumb |
                         layout::kXusbRightThumb | layout::kXusbGuide;
    CHECK(out.wButtons == expected);
    CHECK(out.lt == 0x80);
    CHECK(out.rt == 0x80);
}

TEST_CASE("big-endian words keep their top bit", "[bytes][endian]") {
    const std::array<std::uint8_t, 8> top{0x80, 0x00, 0x00, 0x01, 0xFF, 0x7F, 0x80, 0xFF};
    CHECK(util::readU16Be(top.data()) == 0x8000U);
    CHECK(util::readU16Be(top.data() + 4) == 0xFF7FU);
    CHECK(util::readU32Be(top.data()) == 0x80000001U);
    CHECK(util::readU32Be(top.data() + 4) == 0xFF7F80FFU);
    CHECK(util::readU64Be(top.data()) == 0x80000001FF7F80FFULL);

    std::array<std::uint8_t, 4> out{};
    util::putU32Be(out.data(), 0x80FF7F01U);
    CHECK(out == std::array<std::uint8_t, 4>{0x80, 0xFF, 0x7F, 0x01});
    std::array<std::uint8_t, 2> half{};
    util::putU16Be(half.data(), 0xFF80U);
    CHECK(half == std::array<std::uint8_t, 2>{0xFF, 0x80});
}

TEST_CASE("HID fields extend their sign only when the descriptor says signed", "[bytes][usb-hid]") {
    CHECK(usbhid::signExtend(0x7FU, 1) == 127);
    CHECK(usbhid::signExtend(0x80U, 1) == -128);
    CHECK(usbhid::signExtend(0xFFU, 1) == -1);
    CHECK(usbhid::signExtend(0x8000U, 2) == -32768);
    CHECK(usbhid::signExtend(0x7FFFU, 2) == 32767);
    CHECK(usbhid::signExtend(0x800000U, 3) == -8388608);
    CHECK(usbhid::signExtend(0x80000000U, 4) == static_cast<std::int32_t>(0x80000000U));

    CHECK(usbhid::toSigned(0x800U, 12, -2048) == -2048);
    CHECK(usbhid::toSigned(0x800U, 12, 0) == 2048);
    CHECK(usbhid::toSigned(0xFFFU, 12, -2048) == -1);

    const std::array<std::uint8_t, 4> d{0xFF, 0x80, 0x7F, 0x00};
    CHECK(usbhid::extractBits(d.data(), d.size(), 7, 2) == 0x1U);  // byte 0 bit 7, byte 1 bit 0
    CHECK(usbhid::extractBits(d.data(), d.size(), 15, 2) == 0x3U); // byte 1 bit 7, byte 2 bit 0
    CHECK(usbhid::extractBits(d.data(), d.size(), 0, 16) == 0x80FFU);
    CHECK(usbhid::extractBits(d.data(), d.size(), 0, 32) == 0x007F80FFU);
    CHECK(usbhid::extractBits(d.data(), d.size(), 24, 16) == 0x00U); // stops at the end
}

TEST_CASE("the packed HID word keeps buttons and hat apart at every top bit", "[bytes][layouts]") {
    CHECK(layout::hidButtonsOf(0x000FFFFF) == 0xFFFF);
    CHECK(layout::hidHatOf(0x000FFFFF) == 0xF);
    CHECK(layout::hidHatOf(0x00080000) == 0x8);
    CHECK(layout::hidButtonsOf(0x00080000) == 0);
    CHECK(layout::hidHatOf(0x7FFFFFFF) == 0xF);
    CHECK(layout::hidButtonsOf(0x00008000) == 0x8000);
}
