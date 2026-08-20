// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Each case feeds a synthetic report byte vector built from the documented
// layout. The DS4/DualSense IMU + touchpad offsets come from the public
// hid-playstation layout and have not been checked against a real pad, so these
// pin the OFFSETS and the present/absent gating, not a hardware-true magnitude.

#include "core/input/UsbReportParsers.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

using namespace dish::input::usbparse;
namespace layout = dish::input::layout;

namespace {

ParsedReport decode(HidParser p, const std::vector<std::uint8_t>& buf) {
    ParsedReport out{};
    StickAutoRangeState sticks;
    const bool ok = decodeReport(p, buf.data(), buf.size(), out, sticks);
    out.wButtons = ok ? out.wButtons : 0xDEAD; // sentinel so a false return is visible.
    return out;
}

} // namespace

TEST_CASE("parserForDevice picks the right family by VID PID", "[usb-parsers]") {
    CHECK(parserForDevice(0x054C, 0x0CE6) == HidParser::DualSense);       // DualSense
    CHECK(parserForDevice(0x054C, 0x0DF2) == HidParser::DualSense);       // DualSense Edge
    CHECK(parserForDevice(0x054C, 0x05C4) == HidParser::DualShock4);      // DS4 v1
    CHECK(parserForDevice(0x054C, 0x09CC) == HidParser::DualShock4);      // DS4 v2
    CHECK(parserForDevice(0x057E, 0x2009) == HidParser::SwitchProUsb);    // Switch Pro
    CHECK(parserForDevice(0x2DC8, 0x9015) == HidParser::GenericHid);      // 8BitDo
    CHECK(parserForDevice(0x1234, 0x5678) == HidParser::GenericHid);      // unknown
    CHECK(parserForDevice(0x28DE, 0x1102) == HidParser::SteamController); // wired
    CHECK(parserForDevice(0x28DE, 0x1142) == HidParser::SteamController); // dongle
    CHECK(parserForDevice(0x0E6F, 0x0180) == HidParser::GenericHid);      // PDP Faceoff
}

TEST_CASE("every PDP wired Switch pad carries the Switch button order", "[usb-parsers][pdp]") {
    // The five wired models dish-android validated; 0x0186 (Afterglow
    // Wireless) is excluded on purpose — it speaks the Switch Pro protocol and
    // its USB port is charge-only, so the wired remap would be wrong for it.
    for (const int pid : {0x0180, 0x0181, 0x0184, 0x0185, 0x0187}) {
        CHECK(parserForDevice(0x0E6F, pid) == HidParser::GenericHid);
        CHECK(buttonOrderForDevice(0x0E6F, pid) == ButtonOrder::Switch);
        CHECK(lookupKnownModel(0x0E6F, pid) != nullptr);
    }
    CHECK(buttonOrderForDevice(0x0E6F, 0x0186) == ButtonOrder::Western);
    CHECK(lookupKnownModel(0x0E6F, 0x0186) == nullptr);
    // Table and vendor fallbacks stay western too.
    CHECK(buttonOrderForDevice(0x054C, 0x0CE6) == ButtonOrder::Western);
    CHECK(buttonOrderForDevice(0x1234, 0x5678) == ButtonOrder::Western);
}

TEST_CASE("only Steam models settle without a framework gamepad", "[usb-parsers][steam]") {
    // A released Steam Controller settles as a keyboard/mouse, never an SDL
    // gamepad; every other model (and anything unknown) keeps the
    // wait-for-re-enumeration contract.
    CHECK_FALSE(modelExpectsFrameworkGamepad(0x28DE, 0x1102));
    CHECK_FALSE(modelExpectsFrameworkGamepad(0x28DE, 0x1142));
    CHECK(modelExpectsFrameworkGamepad(0x045E, 0x028E));
    CHECK(modelExpectsFrameworkGamepad(0x054C, 0x05C4));
    CHECK(modelExpectsFrameworkGamepad(0x0E6F, 0x0180));
    CHECK(modelExpectsFrameworkGamepad(0x1234, 0x5678));
}

TEST_CASE("parser capability predicates agree with the families", "[usb-parsers]") {
    CHECK(parserHasImu(HidParser::DualShock4));
    CHECK(parserHasImu(HidParser::DualSense));
    CHECK(parserHasImu(HidParser::SwitchProUsb));
    CHECK(parserHasImu(HidParser::SteamController));
    CHECK_FALSE(parserHasImu(HidParser::GenericHid));
    CHECK_FALSE(parserHasImu(HidParser::None));
    CHECK(parserHasRumble(HidParser::DualShock4));
    CHECK(parserHasRumble(HidParser::DualSense));
    CHECK(parserHasRumble(HidParser::SwitchProUsb));
    // No rumble motors, only trackpad voice coils; the simple-rumble command is
    // Steam Deck firmware only.
    CHECK_FALSE(parserHasRumble(HidParser::SteamController));
    CHECK_FALSE(parserHasRumble(HidParser::GenericHid));
    CHECK(parserHasTouchpad(HidParser::DualShock4));
    CHECK(parserHasTouchpad(HidParser::DualSense));
    CHECK_FALSE(parserHasTouchpad(HidParser::SwitchProUsb));
    CHECK_FALSE(parserHasTouchpad(HidParser::SteamController));
    CHECK_FALSE(parserHasTouchpad(HidParser::GenericHid));
}

TEST_CASE("DualShock4 decodes Cross to XUSB A and centers neutral sticks", "[usb-parsers][ds4]") {
    std::vector<std::uint8_t> r(10, 0);
    r[0] = 0x01;
    r[1] = 128; // LX center
    r[2] = 128; // LY center
    r[3] = 128; // RX center
    r[4] = 128; // RY center
    r[5] = 0x20;
    // Byte 5 low nibble is the hat octant, so it must be 8 (neutral) for a pure
    // face-button check; 0 would read as Up.
    r[5] = static_cast<std::uint8_t>(0x20 | 0x08); // Cross + hat neutral.
    const auto out = decode(HidParser::DualShock4, r);
    CHECK((out.wButtons & layout::kXusbA) != 0);
    CHECK((out.wButtons & layout::kXusbDpadUp) == 0);
    CHECK(out.lx == 0);
    CHECK(out.ly == 0);
    CHECK(out.rx == 0);
    CHECK(out.ry == 0);
}

TEST_CASE("DualShock4 maps every face and shoulder button by position", "[usb-parsers][ds4]") {
    std::vector<std::uint8_t> r(10, 0);
    r[0] = 0x01;
    r[1] = r[2] = r[3] = r[4] = 128;
    r[5] = 0x08;                                    // hat neutral, no face bits.
    r[5] |= 0x10;                                   // Square -> X
    r[5] |= 0x80;                                   // Triangle -> Y
    r[6] = 0x01 | 0x02 | 0x10 | 0x20 | 0x40 | 0x80; // L1,R1,Share,Options,L3,R3
    const auto out = decode(HidParser::DualShock4, r);
    CHECK((out.wButtons & layout::kXusbX) != 0);
    CHECK((out.wButtons & layout::kXusbY) != 0);
    CHECK((out.wButtons & layout::kXusbLeftShoulder) != 0);
    CHECK((out.wButtons & layout::kXusbRightShoulder) != 0);
    CHECK((out.wButtons & layout::kXusbBack) != 0);  // Share
    CHECK((out.wButtons & layout::kXusbStart) != 0); // Options
    CHECK((out.wButtons & layout::kXusbLeftThumb) != 0);
    CHECK((out.wButtons & layout::kXusbRightThumb) != 0);
}

TEST_CASE("DualShock4 inverts the Y axis and scales triggers", "[usb-parsers][ds4]") {
    std::vector<std::uint8_t> r(10, 0);
    r[0] = 0x01;
    r[1] = 255; // LX full right -> large positive
    r[2] = 0;   // LY raw min; inverted -> large positive
    r[3] = 128;
    r[4] = 255; // RY raw max; inverted -> large negative
    r[5] = 0x08;
    r[8] = 200; // L2
    r[9] = 50;  // R2
    const auto out = decode(HidParser::DualShock4, r);
    CHECK(out.lx > 30000);  // 255 -> +32639
    CHECK(out.ly > 30000);  // inverted 0 -> +32896 clamped to +32767
    CHECK(out.ry < -30000); // inverted 255 -> -32639
    CHECK(out.lt == 200);
    CHECK(out.rt == 50);
}

TEST_CASE("DualShock4 decodes the hat to dpad bits", "[usb-parsers][ds4]") {
    std::vector<std::uint8_t> r(10, 0);
    r[0] = 0x01;
    r[1] = r[2] = r[3] = r[4] = 128;
    r[5] = 0x02; // hat octant 2 = East -> dpad right
    const auto out = decode(HidParser::DualShock4, r);
    CHECK((out.wButtons & layout::kXusbDpadRight) != 0);
    CHECK((out.wButtons & layout::kXusbDpadUp) == 0);
}

TEST_CASE("DualShock4 a short report has no IMU or touchpad", "[usb-parsers][ds4]") {
    std::vector<std::uint8_t> r(10, 0);
    r[0] = 0x01;
    r[1] = r[2] = r[3] = r[4] = 128;
    r[5] = 0x08;
    const auto out = decode(HidParser::DualShock4, r);
    CHECK(out.wButtons != 0xDEAD);
    CHECK_FALSE(out.motionValid);
    CHECK_FALSE(out.touchpadValid);
}

TEST_CASE("DualShock4 a full report carries IMU and touchpad", "[usb-parsers][ds4]") {
    std::vector<std::uint8_t> r(64, 0);
    r[0] = 0x01;
    r[1] = r[2] = r[3] = r[4] = 128;
    r[5] = 0x08;
    // gyro X at 13/14 (LE 0x0100 = 256), accel X at 19/20 (LE 0x0010 = 16).
    r[13] = 0x00;
    r[14] = 0x01;
    r[19] = 0x10;
    r[20] = 0x00;
    // Touch finger 0 at byte 35: id 3, touching (bit7 clear), x=0x010, y=0x020.
    r[35] = 0x03;
    r[36] = 0x10; // x low
    r[37] = 0x20; // x high nibble 0, y low nibble 2
    r[38] = 0x00; // y high
    // Finger 1 not touching (bit7 set).
    r[39] = 0x80;
    const auto out = decode(HidParser::DualShock4, r);
    REQUIRE(out.motionValid);
    CHECK(out.gyroX != 0);
    CHECK(out.accelX != 0);
    REQUIRE(out.touchpadValid);
    CHECK(out.finger0Active);
    CHECK(out.finger0Id == 3);
    CHECK_FALSE(out.finger1Active);
}

TEST_CASE("DualShock4 rejects a wrong report id", "[usb-parsers][ds4]") {
    std::vector<std::uint8_t> r(10, 0);
    r[0] = 0x11; // not 0x01
    const auto out = decode(HidParser::DualShock4, r);
    CHECK(out.wButtons == 0xDEAD);
}

TEST_CASE("DualSense decodes Cross to XUSB A with shifted trigger and button bytes",
          "[usb-parsers][dualsense]") {
    std::vector<std::uint8_t> r(11, 0);
    r[0] = 0x01;
    r[1] = r[2] = r[3] = r[4] = 128;
    r[5] = 150;                                    // L2 analog at byte 5 (DualSense, not byte 8)
    r[6] = 40;                                     // R2 analog at byte 6
    r[8] = static_cast<std::uint8_t>(0x20 | 0x08); // Cross -> A, hat neutral
    r[9] = 0x20;                                   // Options -> Start
    const auto out = decode(HidParser::DualSense, r);
    CHECK((out.wButtons & layout::kXusbA) != 0);
    CHECK((out.wButtons & layout::kXusbStart) != 0);
    CHECK(out.lt == 150);
    CHECK(out.rt == 40);
    CHECK(out.lx == 0);
}

TEST_CASE("DualSense maps shoulders thumbs and Share by position", "[usb-parsers][dualsense]") {
    std::vector<std::uint8_t> r(11, 0);
    r[0] = 0x01;
    r[1] = r[2] = r[3] = r[4] = 128;
    r[8] = 0x08;                             // hat neutral
    r[8] |= 0x40;                            // Circle -> B
    r[9] = 0x01 | 0x02 | 0x10 | 0x40 | 0x80; // L1,R1,Create,L3,R3
    const auto out = decode(HidParser::DualSense, r);
    CHECK((out.wButtons & layout::kXusbB) != 0);
    CHECK((out.wButtons & layout::kXusbLeftShoulder) != 0);
    CHECK((out.wButtons & layout::kXusbRightShoulder) != 0);
    CHECK((out.wButtons & layout::kXusbBack) != 0); // Create
    CHECK((out.wButtons & layout::kXusbLeftThumb) != 0);
    CHECK((out.wButtons & layout::kXusbRightThumb) != 0);
}

TEST_CASE("DualSense a full report carries IMU and touchpad", "[usb-parsers][dualsense]") {
    std::vector<std::uint8_t> r(48, 0);
    r[0] = 0x01;
    r[1] = r[2] = r[3] = r[4] = 128;
    r[8] = 0x08;
    r[16] = 0x00; // gyro X LE
    r[17] = 0x02; // 0x0200
    r[22] = 0x40; // accel X LE
    r[23] = 0x00; // 0x0040
    r[33] = 0x05; // touch finger 0 id 5, touching
    r[34] = 0x20;
    r[35] = 0x10;
    r[36] = 0x00;
    r[37] = 0x80; // finger 1 not touching
    const auto out = decode(HidParser::DualSense, r);
    REQUIRE(out.motionValid);
    CHECK(out.gyroX != 0);
    CHECK(out.accelX != 0);
    REQUIRE(out.touchpadValid);
    CHECK(out.finger0Active);
    CHECK(out.finger0Id == 5);
    CHECK_FALSE(out.finger1Active);
}

TEST_CASE("SwitchPro maps face buttons by physical position and digital triggers",
          "[usb-parsers][switch]") {
    std::vector<std::uint8_t> r(25, 0);
    r[0] = 0x30;
    // right byte: bit0 X(->XUSB_X), bit2 A(->XUSB_A), bit3 B(->XUSB_B), bit6 R, bit7 ZR
    r[3] = 0x01 | 0x04 | 0x80;
    // shared: bit1 Plus(->Start), bit4 Home(->Guide)
    r[4] = 0x02 | 0x10;
    // left: bit1 Up(->dpad up), bit6 L, bit7 ZL
    r[5] = 0x02 | 0x80;
    // sticks centered (2048 packed): LX=2048 -> buf6=0x00, buf7 low nibble=0x08
    r[6] = 0x00;
    r[7] = 0x80; // ly and lx share this byte's nibbles
    r[8] = 0x08;
    r[9] = 0x00;
    r[10] = 0x80;
    r[11] = 0x08;
    const auto out = decode(HidParser::SwitchProUsb, r);
    CHECK((out.wButtons & layout::kXusbX) != 0);
    CHECK((out.wButtons & layout::kXusbA) != 0);
    CHECK((out.wButtons & layout::kXusbStart) != 0);
    CHECK((out.wButtons & layout::kXusbGuide) != 0);
    CHECK((out.wButtons & layout::kXusbDpadUp) != 0);
    CHECK(out.rt == 255); // ZR digital -> full
    CHECK(out.lt == 255); // ZL digital -> full
}

TEST_CASE("SwitchPro a full report decodes the first IMU frame", "[usb-parsers][switch]") {
    std::vector<std::uint8_t> r(25, 0);
    r[0] = 0x30;
    // accel X at 13/14 (LE 0x0100), gyro X at 19/20 (LE 0x0080).
    r[13] = 0x00;
    r[14] = 0x01;
    r[19] = 0x80;
    r[20] = 0x00;
    const auto out = decode(HidParser::SwitchProUsb, r);
    REQUIRE(out.motionValid);
    CHECK(out.accelX != 0);
    CHECK(out.gyroX != 0);
}

TEST_CASE("SwitchPro rejects a short report", "[usb-parsers][switch]") {
    std::vector<std::uint8_t> r(11, 0);
    r[0] = 0x30;
    const auto out = decode(HidParser::SwitchProUsb, r);
    CHECK(out.wButtons == 0xDEAD);
}

TEST_CASE("GenericHid decodes axes hat face and shoulder buttons", "[usb-parsers][generic]") {
    std::vector<std::uint8_t> r(7, 0);
    r[0] = 128;                                           // LX center
    r[1] = 128;                                           // LY center
    r[2] = 128;                                           // RX center
    r[3] = 128;                                           // RY center
    r[4] = static_cast<std::uint8_t>(0x08 | 0x10 | 0x20); // hat neutral + A + B
    r[5] = 0x01 | 0x02 | 0x40 | 0x80;                     // LB, RB, LT, RT
    const auto out = decode(HidParser::GenericHid, r);
    CHECK((out.wButtons & layout::kXusbA) != 0);
    CHECK((out.wButtons & layout::kXusbB) != 0);
    CHECK((out.wButtons & layout::kXusbLeftShoulder) != 0);
    CHECK((out.wButtons & layout::kXusbRightShoulder) != 0);
    CHECK(out.lt == 255);
    CHECK(out.rt == 255);
    CHECK(out.lx == 0);
}

TEST_CASE("GenericHid never reports IMU or touchpad", "[usb-parsers][generic]") {
    std::vector<std::uint8_t> r(7, 0);
    r[0] = r[1] = r[2] = r[3] = 128;
    r[4] = 0x08;
    const auto out = decode(HidParser::GenericHid, r);
    CHECK_FALSE(out.motionValid);
    CHECK_FALSE(out.touchpadValid);
}

TEST_CASE("GenericHid rejects a too-short report", "[usb-parsers][generic]") {
    std::vector<std::uint8_t> r(6, 0);
    const auto out = decode(HidParser::GenericHid, r);
    CHECK(out.wButtons == 0xDEAD);
}

TEST_CASE("scaleU8Centered centers and inverts", "[usb-parsers][scale]") {
    CHECK(scaleU8Centered(128, false) == 0);
    CHECK(scaleU8Centered(128, true) == 0);
    CHECK(scaleU8Centered(255, false) > 32000);
    CHECK(scaleU8Centered(0, false) < -32000);
    CHECK(scaleU8Centered(0, true) > 32000);
    CHECK(scaleU8Centered(255, true) < -32000);
}

TEST_CASE("scaleSwitchStickAuto deadzones the center and ranges the throw",
          "[usb-parsers][scale]") {
    AxisAutoRange axis;
    CHECK(scaleSwitchStickAuto(2048, axis) == 0);                 // dead center
    CHECK(scaleSwitchStickAuto(2048 + 100, axis) == 0);           // inside deadzone (<=320)
    const std::int16_t pushed = scaleSwitchStickAuto(4095, axis); // full positive
    CHECK(pushed > 0);
    const std::int16_t pulled = scaleSwitchStickAuto(0, axis); // full negative
    CHECK(pulled < 0);
}

// ── Steam Controller ─────────────────────────────────────────────────────────
// Vectors mirrored 1:1 from dish-android's usb_parsers_test.cpp (#154), whose
// decode was hardware-verified against a wired 28DE:1102.

namespace {

constexpr std::uint32_t kBtnRightBumper = 0x000004;
constexpr std::uint32_t kBtnLeftBumper = 0x000008;
constexpr std::uint32_t kBtnNorth = 0x000010;
constexpr std::uint32_t kBtnEast = 0x000020;
constexpr std::uint32_t kBtnWest = 0x000040;
constexpr std::uint32_t kBtnSouth = 0x000080;
constexpr std::uint32_t kBtnDpadUp = 0x000100;
constexpr std::uint32_t kBtnDpadRight = 0x000200;
constexpr std::uint32_t kBtnMenu = 0x001000;
constexpr std::uint32_t kBtnGuide = 0x002000;
constexpr std::uint32_t kBtnEscape = 0x004000;
constexpr std::uint32_t kBtnLeftPadClicked = 0x020000;
constexpr std::uint32_t kBtnRightPadClicked = 0x040000;
constexpr std::uint32_t kBtnLeftPadFinger = 0x080000;
constexpr std::uint32_t kBtnRightPadFinger = 0x100000;
constexpr std::uint32_t kBtnStickButton = 0x400000;
constexpr std::uint32_t kBtnLeftPadAndStick = 0x800000;

std::vector<std::uint8_t> steamState(std::uint32_t buttons = 0) {
    std::vector<std::uint8_t> r(64, 0);
    r[0] = 0x01; // report version, u16 LE
    r[1] = 0x00;
    r[2] = 0x01; // ID_CONTROLLER_STATE
    r[3] = 44;   // payload length
    r[8] = static_cast<std::uint8_t>(buttons & 0xFF);
    r[9] = static_cast<std::uint8_t>((buttons >> 8) & 0xFF);
    r[10] = static_cast<std::uint8_t>((buttons >> 16) & 0xFF);
    return r;
}

void setLe16(std::vector<std::uint8_t>& r, std::size_t off, std::int16_t v) {
    r[off] = static_cast<std::uint8_t>(static_cast<std::uint16_t>(v) & 0xFF);
    r[off + 1] = static_cast<std::uint8_t>(static_cast<std::uint16_t>(v) >> 8);
}

bool decodeSteam(const std::vector<std::uint8_t>& buf, ParsedReport& s, StickAutoRangeState& st) {
    return decodeReport(HidParser::SteamController, buf.data(), buf.size(), s, st);
}

// Dongle wireless event framing per hid-steam: header {version u16, type,
// payload length}, payload byte 0x01 = disconnected, 0x02 = connected.
std::vector<std::uint8_t> steamWirelessEvent(std::uint8_t payload) {
    std::vector<std::uint8_t> r(64, 0);
    r[0] = 0x01;
    r[1] = 0x00;
    r[2] = 0x03; // ID_CONTROLLER_WIRELESS
    r[3] = 0x01; // payload length
    r[4] = payload;
    return r;
}

} // namespace

TEST_CASE("Steam rejects short wrong-version and wrong-type packets", "[usb-parsers][steam]") {
    StickAutoRangeState st;
    ParsedReport s{};
    auto shortPacket = steamState();
    shortPacket.resize(47);
    CHECK_FALSE(decodeSteam(shortPacket, s, st));

    auto badVersion = steamState();
    badVersion[0] = 0x02;
    CHECK_FALSE(decodeSteam(badVersion, s, st));

    // The dongle interleaves wireless connect/disconnect events onto the same
    // endpoint.
    auto wirelessEvent = steamState();
    wirelessEvent[2] = 0x03;
    CHECK_FALSE(decodeSteam(wirelessEvent, s, st));
}

TEST_CASE("Steam face and shoulder buttons map by position", "[usb-parsers][steam]") {
    StickAutoRangeState st;
    ParsedReport s{};
    REQUIRE(decodeSteam(steamState(kBtnSouth | kBtnEast | kBtnWest | kBtnNorth), s, st));
    CHECK((s.wButtons & layout::kXusbA) != 0);
    CHECK((s.wButtons & layout::kXusbB) != 0);
    CHECK((s.wButtons & layout::kXusbX) != 0);
    CHECK((s.wButtons & layout::kXusbY) != 0);

    ParsedReport shoulders{};
    REQUIRE(decodeSteam(steamState(kBtnLeftBumper | kBtnRightBumper), shoulders, st));
    CHECK((shoulders.wButtons & layout::kXusbLeftShoulder) != 0);
    CHECK((shoulders.wButtons & layout::kXusbRightShoulder) != 0);
}

TEST_CASE("Steam menu escape and guide map to back start guide", "[usb-parsers][steam]") {
    StickAutoRangeState st;
    ParsedReport s{};
    REQUIRE(decodeSteam(steamState(kBtnMenu | kBtnEscape | kBtnGuide), s, st));
    CHECK((s.wButtons & layout::kXusbBack) != 0);
    CHECK((s.wButtons & layout::kXusbStart) != 0);
    CHECK((s.wButtons & layout::kXusbGuide) != 0);
}

TEST_CASE("Steam dpad bits map directly", "[usb-parsers][steam]") {
    StickAutoRangeState st;
    ParsedReport s{};
    REQUIRE(decodeSteam(steamState(kBtnDpadUp | kBtnDpadRight), s, st));
    CHECK((s.wButtons & layout::kXusbDpadUp) != 0);
    CHECK((s.wButtons & layout::kXusbDpadRight) != 0);
    CHECK((s.wButtons & layout::kXusbDpadDown) == 0);
    CHECK((s.wButtons & layout::kXusbDpadLeft) == 0);
}

TEST_CASE("Steam triggers scale and saturate before the raw rail", "[usb-parsers][steam]") {
    StickAutoRangeState st;
    ParsedReport s{};
    auto r = steamState();
    r[11] = 0;
    r[12] = 100;
    REQUIRE(decodeSteam(r, s, st));
    CHECK(s.lt == 0);
    CHECK(s.rt == 126);

    // Valve full scale is 26000 of a possible 32895, so the throw tops out
    // before the raw rail.
    r[11] = 202;
    r[12] = 255;
    REQUIRE(decodeSteam(r, s, st));
    CHECK(s.lt == 255);
    CHECK(s.rt == 255);
}

TEST_CASE("Steam left axes are the stick when no finger is on the pad", "[usb-parsers][steam]") {
    StickAutoRangeState st;
    ParsedReport s{};
    auto r = steamState();
    setLe16(r, 16, -8000);
    setLe16(r, 18, 12000);
    REQUIRE(decodeSteam(r, s, st));
    CHECK(s.lx == -8000);
    CHECK(s.ly == 12000);
}

TEST_CASE("Steam stick holds its value across an interleaved pad frame", "[usb-parsers][steam]") {
    StickAutoRangeState st;
    ParsedReport s{};
    auto stickFrame = steamState();
    setLe16(stickFrame, 16, 5000);
    setLe16(stickFrame, 18, -6000);
    REQUIRE(decodeSteam(stickFrame, s, st));

    auto padFrame = steamState(kBtnLeftPadFinger | kBtnLeftPadAndStick);
    setLe16(padFrame, 16, 30000);
    setLe16(padFrame, 18, 30000);
    ParsedReport next{};
    REQUIRE(decodeSteam(padFrame, next, st));
    CHECK(next.lx == 5000);
    CHECK(next.ly == -6000);
}

TEST_CASE("Steam stick centres when the pad takes over without interleaving",
          "[usb-parsers][steam]") {
    StickAutoRangeState st;
    ParsedReport s{};
    auto stickFrame = steamState();
    setLe16(stickFrame, 16, 5000);
    REQUIRE(decodeSteam(stickFrame, s, st));

    auto padFrame = steamState(kBtnLeftPadFinger);
    setLe16(padFrame, 16, 30000);
    ParsedReport next{};
    REQUIRE(decodeSteam(padFrame, next, st));
    CHECK(next.lx == 0);
    CHECK(next.ly == 0);
}

TEST_CASE("Steam left-pad click is a stick click while the pad is idle", "[usb-parsers][steam]") {
    StickAutoRangeState st;
    ParsedReport s{};
    REQUIRE(decodeSteam(steamState(kBtnLeftPadClicked), s, st));
    CHECK((s.wButtons & layout::kXusbLeftThumb) != 0);

    // With a finger actually on the pad the click belongs to the pad, not the
    // stick.
    ParsedReport onPad{};
    REQUIRE(decodeSteam(steamState(kBtnLeftPadClicked | kBtnLeftPadFinger), onPad, st));
    CHECK((onPad.wButtons & layout::kXusbLeftThumb) == 0);
}

TEST_CASE("Steam stick and right-pad clicks map to thumb buttons", "[usb-parsers][steam]") {
    StickAutoRangeState st;
    ParsedReport s{};
    REQUIRE(decodeSteam(steamState(kBtnStickButton | kBtnRightPadClicked), s, st));
    CHECK((s.wButtons & layout::kXusbLeftThumb) != 0);
    CHECK((s.wButtons & layout::kXusbRightThumb) != 0);
}

TEST_CASE("Steam right pad drives the right stick through the shell rotation",
          "[usb-parsers][steam]") {
    StickAutoRangeState st;
    ParsedReport s{};
    auto r = steamState(kBtnRightPadFinger);
    setLe16(r, 20, 10000);
    setLe16(r, 22, 0);
    REQUIRE(decodeSteam(r, s, st));
    CHECK(s.rx == 9659);
    CHECK(s.ry == 2588);
}

TEST_CASE("Steam right stick recentres when the finger lifts", "[usb-parsers][steam]") {
    StickAutoRangeState st;
    ParsedReport s{};
    auto held = steamState(kBtnRightPadFinger);
    setLe16(held, 20, 20000);
    setLe16(held, 22, 20000);
    REQUIRE(decodeSteam(held, s, st));
    REQUIRE(s.rx != 0);

    // Pad coordinates are meaningless once the finger lifts, so the stick must
    // not stay deflected.
    auto lifted = steamState();
    setLe16(lifted, 20, 20000);
    setLe16(lifted, 22, 20000);
    ParsedReport next{};
    REQUIRE(decodeSteam(lifted, next, st));
    CHECK(next.rx == 0);
    CHECK(next.ry == 0);
}

TEST_CASE("Steam IMU rotates onto the wire axis order and scale", "[usb-parsers][steam]") {
    StickAutoRangeState st;
    ParsedReport s{};
    auto r = steamState();
    setLe16(r, 28, 1000); // accel x
    setLe16(r, 30, 2000); // accel y
    setLe16(r, 32, 3000); // accel z
    setLe16(r, 34, 4000); // gyro x
    setLe16(r, 36, 5000); // gyro y
    setLe16(r, 38, 6000); // gyro z
    REQUIRE(decodeSteam(r, s, st));
    CHECK(s.motionValid);
    CHECK(s.gyroX == 3999);
    CHECK(s.gyroY == 5999);
    CHECK(s.gyroZ == 4999);
    CHECK(s.accelX == 499);
    CHECK(s.accelY == 1499);
    CHECK(s.accelZ == -999);
}

TEST_CASE("Steam silent IMU block does not publish motion", "[usb-parsers][steam]") {
    StickAutoRangeState st;
    ParsedReport s{};
    REQUIRE(decodeSteam(steamState(kBtnSouth), s, st));
    CHECK_FALSE(s.motionValid);
}

TEST_CASE("Steam quiet sequence clears mappings then writes settings", "[usb-parsers][steam]") {
    std::uint8_t buf[16];
    REQUIRE(buildSteamConfigPacket(SteamConfig::Quiet, 0, buf, sizeof(buf)) == 2);
    CHECK(buf[0] == 0x81);
    CHECK(buf[1] == 0x00);

    REQUIRE(buildSteamConfigPacket(SteamConfig::Quiet, 1, buf, sizeof(buf)) == 11);
    CHECK(buf[0] == 0x87);
    CHECK(buf[1] == 0x09);
    CHECK(buf[2] == 0x07); // left trackpad mode
    CHECK(buf[3] == 0x07); // = none
    CHECK(buf[5] == 0x08); // right trackpad mode
    CHECK(buf[6] == 0x07); // = none
    CHECK(buf[8] == 0x30); // imu mode
    CHECK(buf[9] == 0x18); // = raw accel | raw gyro

    CHECK(buildSteamConfigPacket(SteamConfig::Quiet, 2, buf, sizeof(buf)) == 0);
}

TEST_CASE("Steam restore sequence puts the device back", "[usb-parsers][steam]") {
    std::uint8_t buf[16];
    REQUIRE(buildSteamConfigPacket(SteamConfig::Restore, 0, buf, sizeof(buf)) == 2);
    CHECK(buf[0] == 0x85);
    REQUIRE(buildSteamConfigPacket(SteamConfig::Restore, 1, buf, sizeof(buf)) == 2);
    CHECK(buf[0] == 0x8E);

    // Loading the defaults leaves the right pad silent, so mouse mode is
    // restored by name.
    REQUIRE(buildSteamConfigPacket(SteamConfig::Restore, 2, buf, sizeof(buf)) == 5);
    CHECK(buf[0] == 0x87);
    CHECK(buf[1] == 0x03);
    CHECK(buf[2] == 0x08); // right trackpad mode
    CHECK(buf[3] == 0x00); // = absolute mouse
    CHECK(buf[4] == 0x00);

    CHECK(buildSteamConfigPacket(SteamConfig::Restore, 3, buf, sizeof(buf)) == 0);
}

TEST_CASE("Steam config packets refuse to overrun a caller buffer", "[usb-parsers][steam]") {
    std::uint8_t small[4];
    CHECK(buildSteamConfigPacket(SteamConfig::Quiet, 1, small, sizeof(small)) == 0);
    CHECK(buildSteamConfigPacket(SteamConfig::Quiet, -1, small, sizeof(small)) == 0);
}

TEST_CASE("Steam wireless connect and disconnect events classify", "[usb-parsers][steam]") {
    const auto disc = steamWirelessEvent(0x01);
    CHECK(checkWirelessEvent(HidParser::SteamController, disc.data(), disc.size()) ==
          WirelessEvent::Disconnect);

    const auto conn = steamWirelessEvent(0x02);
    CHECK(checkWirelessEvent(HidParser::SteamController, conn.data(), conn.size()) ==
          WirelessEvent::Connect);
}

TEST_CASE("Steam wireless event never decodes as input but still classifies",
          "[usb-parsers][steam]") {
    // The issue and the fix as a pair: on its own the last published state
    // would stay latched; the classifier is what routes it to the gateway.
    StickAutoRangeState st;
    ParsedReport s{};
    const auto disc = steamWirelessEvent(0x01);
    CHECK_FALSE(decodeReport(HidParser::SteamController, disc.data(), disc.size(), s, st));
    CHECK(checkWirelessEvent(HidParser::SteamController, disc.data(), disc.size()) !=
          WirelessEvent::None);
}

TEST_CASE("Steam input state is not a wireless event", "[usb-parsers][steam]") {
    StickAutoRangeState st;
    ParsedReport s{};
    const auto state = steamState(kBtnSouth);
    CHECK(checkWirelessEvent(HidParser::SteamController, state.data(), state.size()) ==
          WirelessEvent::None);
    CHECK(decodeReport(HidParser::SteamController, state.data(), state.size(), s, st));
}

TEST_CASE("Steam wireless classifier rejects malformed events", "[usb-parsers][steam]") {
    auto shortEvent = steamWirelessEvent(0x02);
    shortEvent.resize(4);
    CHECK(checkWirelessEvent(HidParser::SteamController, shortEvent.data(), shortEvent.size()) ==
          WirelessEvent::None);

    auto badVersion = steamWirelessEvent(0x02);
    badVersion[0] = 0x02;
    CHECK(checkWirelessEvent(HidParser::SteamController, badVersion.data(), badVersion.size()) ==
          WirelessEvent::None);

    auto badPayloadLen = steamWirelessEvent(0x02);
    badPayloadLen[3] = 0x02;
    CHECK(checkWirelessEvent(HidParser::SteamController, badPayloadLen.data(),
                             badPayloadLen.size()) == WirelessEvent::None);

    const auto unknownPayload = steamWirelessEvent(0x03);
    CHECK(checkWirelessEvent(HidParser::SteamController, unknownPayload.data(),
                             unknownPayload.size()) == WirelessEvent::None);

    // ID_CONTROLLER_STATUS (battery) shares the endpoint but is not a connect
    // event.
    auto battery = steamWirelessEvent(0x02);
    battery[2] = 0x04;
    CHECK(checkWirelessEvent(HidParser::SteamController, battery.data(), battery.size()) ==
          WirelessEvent::None);
}

TEST_CASE("other parsers never see wireless events", "[usb-parsers][steam]") {
    const auto conn = steamWirelessEvent(0x02);
    CHECK(checkWirelessEvent(HidParser::DualSense, conn.data(), conn.size()) ==
          WirelessEvent::None);
    CHECK(checkWirelessEvent(HidParser::GenericHid, conn.data(), conn.size()) ==
          WirelessEvent::None);
    CHECK(checkWirelessEvent(HidParser::None, conn.data(), conn.size()) == WirelessEvent::None);
}
