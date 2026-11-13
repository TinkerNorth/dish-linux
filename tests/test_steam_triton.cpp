// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The 2026 Steam Controller ("Triton") on the Direct path. Every report below is
// built from the packed structs in SDL's steam/controller_structs.h
// (TritonMTUNoQuat_t, TritonMTUNoQuat32TS_t, TritonBatteryStatus_t,
// TritonWirelessStatus_t, FeatureReportMsg, OutputReportMsg) and the mapping in
// SDL_hidapi_steam_triton.c, not from the decoder: no pad has been on hand, so
// these pin the port to SDL's reading of the protocol, byte for byte.

#include "core/input/UsbOutputReports.h"
#include "core/input/UsbReportParsers.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

using namespace dish::input::usbparse;
namespace layout = dish::input::layout;
namespace usbout = dish::input::usbout;

namespace {

constexpr int kValve = 0x28DE;
constexpr int kTritonWired = 0x1302;
constexpr int kTritonBle = 0x1303;
constexpr int kProteusDongle = 0x1304;
constexpr int kNereidDongle = 0x1305;

// SDL's ETritonReportIDTypes.
constexpr std::uint8_t kStateId = 0x42;
constexpr std::uint8_t kBatteryId = 0x43;
constexpr std::uint8_t kStateBleId = 0x45;
constexpr std::uint8_t kWirelessXId = 0x46;
constexpr std::uint8_t kStateTimestampId = 0x47;
constexpr std::uint8_t kWirelessId = 0x79;

// SDL's TritonButtons.
constexpr std::uint32_t kA = 0x00000001;
constexpr std::uint32_t kB = 0x00000002;
constexpr std::uint32_t kX = 0x00000004;
constexpr std::uint32_t kY = 0x00000008;
constexpr std::uint32_t kQam = 0x00000010;
constexpr std::uint32_t kR3 = 0x00000020;
constexpr std::uint32_t kView = 0x00000040;
constexpr std::uint32_t kR4 = 0x00000080;
constexpr std::uint32_t kR5 = 0x00000100;
constexpr std::uint32_t kR = 0x00000200;
constexpr std::uint32_t kDpadDown = 0x00000400;
constexpr std::uint32_t kDpadRight = 0x00000800;
constexpr std::uint32_t kDpadLeft = 0x00001000;
constexpr std::uint32_t kDpadUp = 0x00002000;
constexpr std::uint32_t kMenu = 0x00004000;
constexpr std::uint32_t kL3 = 0x00008000;
constexpr std::uint32_t kSteam = 0x00010000;
constexpr std::uint32_t kL4 = 0x00020000;
constexpr std::uint32_t kL5 = 0x00040000;
constexpr std::uint32_t kL = 0x00080000;
// Touches, pad and trigger clicks, grips: bits 20..29.
constexpr std::uint32_t kRightStickTouch = 0x00100000;
constexpr std::uint32_t kRightPadTouch = 0x00200000;
constexpr std::uint32_t kRightPadClick = 0x00400000;
constexpr std::uint32_t kRightTriggerClick = 0x00800000;
constexpr std::uint32_t kLeftStickTouch = 0x01000000;
constexpr std::uint32_t kLeftPadTouch = 0x02000000;
constexpr std::uint32_t kLeftPadClick = 0x04000000;
constexpr std::uint32_t kLeftTriggerClick = 0x08000000;
constexpr std::uint32_t kRightGripTouch = 0x10000000;
constexpr std::uint32_t kLeftGripTouch = 0x20000000;
constexpr std::uint32_t kEverySenseAndClick = 0x3FF00000;

// One state report, 1 id byte + the 45-byte packed body both layouts share.
constexpr std::size_t kStateLen = 46;

void putLe16(std::vector<std::uint8_t>& r, std::size_t off, std::int16_t v) {
    const auto u = static_cast<std::uint16_t>(v);
    r[off] = static_cast<std::uint8_t>(u & 0xFF);
    r[off + 1] = static_cast<std::uint8_t>(u >> 8);
}

void putLe32(std::vector<std::uint8_t>& r, std::size_t off, std::uint32_t v) {
    for (std::size_t i = 0; i < 4; ++i) { r[off + i] = static_cast<std::uint8_t>(v >> (8 * i)); }
}

// TritonMTUNoQuat_t after the id: seq 1, buttons 2..5, triggers 6/8, sticks
// 10/12/14/16, left pad 18/20 + pressure 22, right pad 24/26 + pressure 28, IMU
// tick u32 at 30, accel 34/36/38, gyro 40/42/44.
std::vector<std::uint8_t> state(std::uint32_t buttons = 0, std::uint8_t id = kStateId) {
    std::vector<std::uint8_t> r(kStateLen, 0);
    r[0] = id;
    putLe32(r, 2, buttons);
    return r;
}

// TritonMTUNoQuat32TS_t: the same up to the sticks, a u16 trackpad timestamp at
// 18 pushes the pads two bytes on, and the IMU tick shrinks to a u16 at 32, so
// the accel and gyro land on the same offsets.
std::vector<std::uint8_t> timestampState(std::uint32_t buttons = 0) {
    return state(buttons, kStateTimestampId);
}

// TritonBatteryStatus_t after the id: charge state, level, then five u16
// voltages and currents and a temperature.
std::vector<std::uint8_t> battery(std::uint8_t chargeState, std::uint8_t level) {
    std::vector<std::uint8_t> r(15, 0);
    r[0] = kBatteryId;
    r[1] = chargeState;
    r[2] = level;
    return r;
}

std::vector<std::uint8_t> wireless(std::uint8_t id, std::uint8_t stateByte) {
    return std::vector<std::uint8_t>{id, stateByte};
}

struct Decoded {
    bool ok = false;
    ParsedReport report{};
};

Decoded decodeTriton(const std::vector<std::uint8_t>& r, StickAutoRangeState& carried) {
    Decoded d;
    d.ok = decodeReport(HidParser::SteamTriton, r.data(), r.size(), d.report, carried);
    return d;
}

Decoded decodeTriton(const std::vector<std::uint8_t>& r) {
    StickAutoRangeState carried;
    return decodeTriton(r, carried);
}

} // namespace

// ── Identity ────────────────────────────────────────────────────────────────

TEST_CASE("the 2026 Steam Controller's wired pad and both dongles are its family",
          "[usb-parsers][triton]") {
    CHECK(parserForDevice(kValve, kTritonWired) == HidParser::SteamTriton);
    CHECK(parserForDevice(kValve, kProteusDongle) == HidParser::SteamTriton);
    CHECK(parserForDevice(kValve, kNereidDongle) == HidParser::SteamTriton);
    // Bluetooth is never Direct, so the BLE identity has no row.
    CHECK(lookupKnownModel(kValve, kTritonBle) == nullptr);
    // The 2015 pad keeps its own family.
    CHECK(parserForDevice(kValve, 0x1102) == HidParser::SteamController);
}

TEST_CASE("a released 2026 Steam Controller settles without a framework gamepad",
          "[usb-parsers][triton]") {
    // Back in lizard mode it is a keyboard and a mouse to the system, like the
    // 2015 pad, so a release must not wait for a gamepad to appear.
    CHECK_FALSE(modelExpectsFrameworkGamepad(kValve, kTritonWired));
    CHECK_FALSE(modelExpectsFrameworkGamepad(kValve, kProteusDongle));
    CHECK_FALSE(modelExpectsFrameworkGamepad(kValve, kNereidDongle));
}

TEST_CASE("the 2026 Steam Controller's surfaces", "[usb-parsers][triton]") {
    CHECK(parserHasImu(HidParser::SteamTriton));
    // Its haptic rumble report drives the two actuators as motors.
    CHECK(parserHasRumble(HidParser::SteamTriton));
    CHECK_FALSE(parserHasTouchpad(HidParser::SteamTriton));
    CHECK_FALSE(parserStreamsUnprompted(HidParser::SteamTriton));
    CHECK_FALSE(parserHasUsbAudio(HidParser::SteamTriton));
    CHECK_FALSE(parserHasHapticLanes(HidParser::SteamTriton));
    CHECK_FALSE(usbout::parserHasLightbar(HidParser::SteamTriton));
    CHECK_FALSE(usbout::parserHasPlayerLeds(HidParser::SteamTriton));
    CHECK_FALSE(usbout::parserHasTriggerEffects(HidParser::SteamTriton));
    CHECK_FALSE(usbout::parserHasMicMuteLed(HidParser::SteamTriton));
}

TEST_CASE("only the two Steam families interleave wireless events with input",
          "[usb-parsers][triton]") {
    CHECK(parserHasWirelessEvents(HidParser::SteamController));
    CHECK(parserHasWirelessEvents(HidParser::SteamTriton));
    CHECK_FALSE(parserHasWirelessEvents(HidParser::DualShock4));
    CHECK_FALSE(parserHasWirelessEvents(HidParser::DualSense));
    CHECK_FALSE(parserHasWirelessEvents(HidParser::SwitchProUsb));
    CHECK_FALSE(parserHasWirelessEvents(HidParser::GenericHid));
    CHECK_FALSE(parserHasWirelessEvents(HidParser::None));
}

TEST_CASE("a dongle's pads ride interfaces 2 to 5, the wired pad any", "[usb-parsers][triton]") {
    for (const int dongle : {kProteusDongle, kNereidDongle}) {
        CHECK(isReceiver(kValve, dongle));
        CHECK_FALSE(interfaceCarriesPad(kValve, dongle, 0));
        CHECK_FALSE(interfaceCarriesPad(kValve, dongle, 1));
        for (int iface = 2; iface <= 5; ++iface) {
            CHECK(interfaceCarriesPad(kValve, dongle, iface));
        }
        CHECK_FALSE(interfaceCarriesPad(kValve, dongle, 6));
    }
    CHECK_FALSE(isReceiver(kValve, kTritonWired));
    CHECK(interfaceCarriesPad(kValve, kTritonWired, 0));
    CHECK(interfaceCarriesPad(kValve, kTritonWired, 2));
}

// ── State reports ───────────────────────────────────────────────────────────

TEST_CASE("a short or foreign report is not the 2026 pad's input", "[usb-parsers][triton]") {
    auto shortState = state(kA);
    shortState.resize(kStateLen - 1);
    CHECK_FALSE(decodeTriton(shortState).ok);
    CHECK_FALSE(decodeTriton(std::vector<std::uint8_t>{}).ok);
    // The 2015 pad's state packet starts with a version word, not a report id.
    std::vector<std::uint8_t> legacy(64, 0);
    legacy[0] = 0x01;
    legacy[2] = 0x01;
    CHECK_FALSE(decodeTriton(legacy).ok);
    CHECK_FALSE(decodeTriton(wireless(kWirelessId, 0x02)).ok);
}

TEST_CASE("the 2026 pad's face, shoulder and stick buttons map by position",
          "[usb-parsers][triton]") {
    const auto d = decodeTriton(state(kA | kB | kX | kY | kL | kR | kL3 | kR3));
    REQUIRE(d.ok);
    const std::uint16_t expected = layout::kXusbA | layout::kXusbB | layout::kXusbX |
                                   layout::kXusbY | layout::kXusbLeftShoulder |
                                   layout::kXusbRightShoulder | layout::kXusbLeftThumb |
                                   layout::kXusbRightThumb;
    CHECK(d.report.wButtons == expected);
}

TEST_CASE("the 2026 pad's menu, view and Steam buttons are back, start and guide",
          "[usb-parsers][triton]") {
    // SDL's mapping, kept as it is: Menu is BACK and View is START.
    CHECK(decodeTriton(state(kMenu)).report.wButtons == layout::kXusbBack);
    CHECK(decodeTriton(state(kView)).report.wButtons == layout::kXusbStart);
    CHECK(decodeTriton(state(kSteam)).report.wButtons == layout::kXusbGuide);
}

TEST_CASE("the 2026 pad's dpad bits map directly", "[usb-parsers][triton]") {
    const auto d = decodeTriton(state(kDpadUp | kDpadDown | kDpadLeft | kDpadRight));
    REQUIRE(d.ok);
    CHECK(d.report.wButtons == (layout::kXusbDpadUp | layout::kXusbDpadDown |
                                layout::kXusbDpadLeft | layout::kXusbDpadRight));
}

TEST_CASE("the buttons with no XUSB bit set nothing", "[usb-parsers][triton]") {
    // The quick-access button, the four back paddles, the pad and trigger clicks
    // and the capacitive touches.
    const auto d = decodeTriton(state(kQam | kL4 | kL5 | kR4 | kR5 | kEverySenseAndClick));
    REQUIRE(d.ok);
    CHECK(d.report.wButtons == 0);
}

TEST_CASE("each extra bit is its SDL-named extended button",
          "[usb-parsers][triton][extended-buttons]") {
    struct Row {
        std::uint32_t triton;
        std::uint32_t ext;
    };
    // SDL_hidapi_steam_triton.c: QAM is misc1, R4/L4/R5/L5 paddle 1-4, the pad
    // clicks touchpad and misc2, the trigger clicks misc3 and misc4, the stick
    // and grip touches the capsense inputs.
    const Row rows[] = {
        {kQam, extbtn::kMisc1},
        {kR4, extbtn::kRightPaddle1},
        {kL4, extbtn::kLeftPaddle1},
        {kR5, extbtn::kRightPaddle2},
        {kL5, extbtn::kLeftPaddle2},
        {kLeftPadClick, extbtn::kLeftPadClick},
        {kRightPadClick, extbtn::kRightPadClick},
        {kLeftTriggerClick, extbtn::kLeftTriggerClick},
        {kRightTriggerClick, extbtn::kRightTriggerClick},
        {kLeftStickTouch, extbtn::kLeftStickTouch},
        {kRightStickTouch, extbtn::kRightStickTouch},
        {kLeftGripTouch, extbtn::kLeftGripTouch},
        {kRightGripTouch, extbtn::kRightGripTouch},
    };
    for (const Row& row : rows) {
        const auto d = decodeTriton(state(row.triton));
        REQUIRE(d.ok);
        CHECK(d.report.extButtons == row.ext);
        CHECK(d.report.wButtons == 0);
    }
}

TEST_CASE("the pad touches and the mapped buttons leave the word clear",
          "[usb-parsers][triton][extended-buttons]") {
    // A finger on a pad has no slot in the word (the pads themselves are the
    // trackpad pick's business), and an XUSB button is only an XUSB button.
    const auto d = decodeTriton(state(kLeftPadTouch | kRightPadTouch | kA | kMenu | kL3 | kDpadUp));
    REQUIRE(d.ok);
    CHECK(d.report.extButtons == 0);
    CHECK((d.report.wButtons & layout::kXusbA) != 0);
}

TEST_CASE("the timestamp and Bluetooth-framed reports carry the same word",
          "[usb-parsers][triton][extended-buttons]") {
    const std::uint32_t held = kQam | kL4 | kRightTriggerClick | kLeftGripTouch;
    const std::uint32_t expected =
        extbtn::kMisc1 | extbtn::kLeftPaddle1 | extbtn::kRightTriggerClick | extbtn::kLeftGripTouch;
    CHECK(decodeTriton(state(held, kStateId)).report.extButtons == expected);
    CHECK(decodeTriton(state(held, kStateBleId)).report.extButtons == expected);
    CHECK(decodeTriton(state(held, kStateTimestampId)).report.extButtons == expected);
}

TEST_CASE("the 2026 pad's triggers scale from their positive half to the 8-bit wire",
          "[usb-parsers][triton]") {
    // SDL sends the trigger as raw * 2 - 32768, so the raw throw is 0..32767.
    auto r = state();
    putLe16(r, 6, 32767);
    putLe16(r, 8, 16384);
    auto d = decodeTriton(r);
    REQUIRE(d.ok);
    CHECK(d.report.lt == 255);
    CHECK(d.report.rt == 127);

    putLe16(r, 6, 0);
    putLe16(r, 8, -5);
    d = decodeTriton(r);
    CHECK(d.report.lt == 0);
    CHECK(d.report.rt == 0);
}

TEST_CASE("the 2026 pad's sticks reach the wire as they are, up positive",
          "[usb-parsers][triton]") {
    // SDL negates Y to make it down-positive; XUSB is up-positive, so the raw
    // value goes as it is.
    auto r = state();
    putLe16(r, 10, 1234);
    putLe16(r, 12, -2345);
    putLe16(r, 14, -32768);
    putLe16(r, 16, 32767);
    const auto d = decodeTriton(r);
    REQUIRE(d.ok);
    CHECK(d.report.lx == 1234);
    CHECK(d.report.ly == -2345);
    CHECK(d.report.rx == -32768);
    CHECK(d.report.ry == 32767);
}

TEST_CASE("the Bluetooth-framed state id decodes like the wired one", "[usb-parsers][triton]") {
    // SDL reads ID_TRITON_CONTROLLER_STATE_BLE with the same struct.
    const auto d = decodeTriton(state(kA, kStateBleId));
    REQUIRE(d.ok);
    CHECK(d.report.wButtons == layout::kXusbA);
}

TEST_CASE("the timestamp layout shares the buttons, triggers and sticks", "[usb-parsers][triton]") {
    auto r = timestampState(kB);
    putLe16(r, 6, 32767);
    putLe16(r, 10, 777);
    putLe16(r, 16, -777);
    const auto d = decodeTriton(r);
    REQUIRE(d.ok);
    CHECK(d.report.wButtons == layout::kXusbB);
    CHECK(d.report.lt == 255);
    CHECK(d.report.lx == 777);
    CHECK(d.report.ry == -777);
}

// ── Motion ──────────────────────────────────────────────────────────────────

TEST_CASE("the 2026 pad's IMU turns onto SDL's sensor axes at the 2015 pad's scale",
          "[usb-parsers][triton]") {
    // SDL: gyro (x, z, -y) over +-2000 deg/s, accel (x, z, -y) over +-2 g. The
    // wire wants deg/s / 2000 and g / 4 at 32767, so a raw count of r becomes
    // r * 32767 / 32768 for the gyro and r * 32767 / 65536 for the accel.
    StickAutoRangeState carried;
    auto r = state();
    putLe32(r, 30, 1000); // the IMU tick
    putLe16(r, 34, 1000); // accel x
    putLe16(r, 36, 2000); // accel y
    putLe16(r, 38, 3000); // accel z
    putLe16(r, 40, 4000); // gyro x
    putLe16(r, 42, 5000); // gyro y
    putLe16(r, 44, 6000); // gyro z
    const auto d = decodeTriton(r, carried);
    REQUIRE(d.ok);
    CHECK(d.report.motionValid);
    CHECK(d.report.gyroX == 3999);
    CHECK(d.report.gyroY == 5999);
    CHECK(d.report.gyroZ == -4999);
    CHECK(d.report.accelX == 499);
    CHECK(d.report.accelY == 1499);
    CHECK(d.report.accelZ == -999);
}

TEST_CASE("the timestamp layout's IMU sits at the same offsets", "[usb-parsers][triton]") {
    StickAutoRangeState carried;
    auto r = timestampState();
    putLe16(r, 32, 7); // the 16-bit IMU tick
    putLe16(r, 40, 4000);
    putLe16(r, 34, 1000);
    const auto d = decodeTriton(r, carried);
    REQUIRE(d.ok);
    CHECK(d.report.motionValid);
    CHECK(d.report.gyroX == 3999);
    CHECK(d.report.accelX == 499);
}

TEST_CASE("the timestamp layout's tick is its own 16-bit field", "[usb-parsers][triton]") {
    // Bytes 30..31 are the right pad's pressure in this layout, not part of the
    // tick: a pressure change under the same tick is no new IMU sample.
    StickAutoRangeState carried;
    auto r = timestampState();
    putLe16(r, 32, 9);
    putLe16(r, 30, 100);
    CHECK(decodeTriton(r, carried).report.motionValid);
    putLe16(r, 30, 200);
    CHECK_FALSE(decodeTriton(r, carried).report.motionValid);
}

TEST_CASE("motion is published only when the IMU tick moves", "[usb-parsers][triton]") {
    // SDL sends a sensor event only for a tick it has not seen; a state report
    // carrying the same IMU sample again is input, not motion.
    StickAutoRangeState carried;
    auto r = state();
    putLe32(r, 30, 500);
    putLe16(r, 40, 4000);
    CHECK(decodeTriton(r, carried).report.motionValid);
    CHECK_FALSE(decodeTriton(r, carried).report.motionValid);
    putLe32(r, 30, 504);
    CHECK(decodeTriton(r, carried).report.motionValid);
}

TEST_CASE("an IMU that never ticked publishes no motion", "[usb-parsers][triton]") {
    // A pad whose IMU setting never took reports tick 0, the value SDL starts
    // from, so it streams no dead sensor.
    const auto d = decodeTriton(state(kA));
    REQUIRE(d.ok);
    CHECK_FALSE(d.report.motionValid);
}

// ── Battery ─────────────────────────────────────────────────────────────────

TEST_CASE("the battery report is not input, and its charge rides the next state report",
          "[usb-parsers][triton][battery]") {
    StickAutoRangeState carried;
    const auto before = decodeTriton(state(kA), carried);
    REQUIRE(before.ok);
    CHECK_FALSE(before.report.batteryValid);

    // k_EChargeStateDischarging, 64 %.
    CHECK_FALSE(decodeTriton(battery(1, 64), carried).ok);

    const auto after = decodeTriton(state(kA), carried);
    REQUIRE(after.ok);
    CHECK(after.report.batteryValid);
    CHECK(after.report.batteryLevel == 64);
    CHECK(after.report.batteryStatus == kPadBatteryStatusDischarging);
    // ...and the input is the state report's own.
    CHECK(after.report.wButtons == layout::kXusbA);
}

TEST_CASE("the 2026 pad's charge states map the way SDL's do", "[usb-parsers][triton][battery]") {
    // EChargeState: 0 reset, 1 discharging, 2 charging, 3 source validate,
    // 4 charging done. SDL calls 0 and 3 unknown and keeps the level.
    struct Case {
        std::uint8_t chargeState;
        std::uint8_t status;
    };
    const Case cases[] = {
        {0, kPadBatteryStatusUnknown},  {1, kPadBatteryStatusDischarging},
        {2, kPadBatteryStatusCharging}, {3, kPadBatteryStatusUnknown},
        {4, kPadBatteryStatusFull},     {9, kPadBatteryStatusUnknown},
    };
    for (const Case& c : cases) {
        INFO("charge state " << static_cast<int>(c.chargeState));
        StickAutoRangeState carried;
        CHECK_FALSE(decodeTriton(battery(c.chargeState, 80), carried).ok);
        const auto d = decodeTriton(state(), carried);
        CHECK(d.report.batteryStatus == c.status);
        CHECK(d.report.batteryLevel == 80);
    }
}

TEST_CASE("a level the wire cannot carry reads as unknown", "[usb-parsers][triton][battery]") {
    StickAutoRangeState carried;
    CHECK_FALSE(decodeTriton(battery(1, 101), carried).ok);
    const auto d = decodeTriton(state(), carried);
    CHECK(d.report.batteryValid);
    CHECK(d.report.batteryLevel == kPadBatteryLevelUnknown);
}

TEST_CASE("a short battery report changes nothing", "[usb-parsers][triton][battery]") {
    StickAutoRangeState carried;
    auto r = battery(1, 50);
    r.resize(14);
    CHECK_FALSE(decodeTriton(r, carried).ok);
    CHECK_FALSE(decodeTriton(state(), carried).report.batteryValid);
}

// ── Dongle connect and disconnect ───────────────────────────────────────────

TEST_CASE("the 2026 dongle's connect and disconnect classify under both ids",
          "[usb-parsers][triton]") {
    for (const std::uint8_t id : {kWirelessXId, kWirelessId}) {
        INFO("report id " << static_cast<int>(id));
        const auto disc = wireless(id, 0x01);
        CHECK(checkWirelessEvent(HidParser::SteamTriton, disc.data(), disc.size()) ==
              WirelessEvent::Disconnect);
        const auto conn = wireless(id, 0x02);
        CHECK(checkWirelessEvent(HidParser::SteamTriton, conn.data(), conn.size()) ==
              WirelessEvent::Connect);
        const auto other = wireless(id, 0x03);
        CHECK(checkWirelessEvent(HidParser::SteamTriton, other.data(), other.size()) ==
              WirelessEvent::None);
    }
}

TEST_CASE("the 2026 classifier refuses short packets, input and the other family's events",
          "[usb-parsers][triton]") {
    const std::vector<std::uint8_t> idOnly{kWirelessId};
    CHECK(checkWirelessEvent(HidParser::SteamTriton, idOnly.data(), idOnly.size()) ==
          WirelessEvent::None);
    const auto input = state(kA);
    CHECK(checkWirelessEvent(HidParser::SteamTriton, input.data(), input.size()) ==
          WirelessEvent::None);
    // The 2015 dongle's event packet, and the 2026 one under the 2015 family.
    const std::vector<std::uint8_t> legacy{0x01, 0x00, 0x03, 0x01, 0x02};
    CHECK(checkWirelessEvent(HidParser::SteamTriton, legacy.data(), legacy.size()) ==
          WirelessEvent::None);
    const auto conn = wireless(kWirelessId, 0x02);
    CHECK(checkWirelessEvent(HidParser::SteamController, conn.data(), conn.size()) ==
          WirelessEvent::None);
}

// ── Settings ────────────────────────────────────────────────────────────────

namespace {

// FeatureReportMsg behind report id 1, as SDL sends it: {id 1, type
// ID_SET_SETTINGS_VALUES (0x87), length 3, setting number, value u16 LE}, then
// zeros to 64 bytes.
void checkSetting(const std::array<std::uint8_t, 64>& report, std::uint8_t setting,
                  std::uint16_t value) {
    CHECK(report[0] == 0x01);
    CHECK(report[1] == 0x87);
    CHECK(report[2] == 0x03);
    CHECK(report[3] == setting);
    CHECK(report[4] == static_cast<std::uint8_t>(value & 0xFF));
    CHECK(report[5] == static_cast<std::uint8_t>(value >> 8));
    for (std::size_t i = 6; i < report.size(); ++i) {
        INFO("byte " << i);
        CHECK(report[i] == 0);
    }
}

constexpr std::uint8_t kSettingLizardMode = 9;
constexpr std::uint8_t kSettingImuMode = 48;

} // namespace

TEST_CASE("the quiet sequence turns lizard mode off, then raw accel and gyro on",
          "[usb-parsers][triton]") {
    std::array<std::uint8_t, 64> report{};
    REQUIRE(buildTritonConfigReport(SteamConfig::Quiet, 0, report.data(), report.size()) == 64);
    checkSetting(report, kSettingLizardMode, 0); // LIZARD_MODE_OFF
    REQUIRE(buildTritonConfigReport(SteamConfig::Quiet, 1, report.data(), report.size()) == 64);
    checkSetting(report, kSettingImuMode, 0x0018); // SEND_RAW_ACCEL | SEND_RAW_GYRO
    CHECK(buildTritonConfigReport(SteamConfig::Quiet, 2, report.data(), report.size()) == 0);
}

TEST_CASE("the restore sequence turns the IMU off and lizard mode back on",
          "[usb-parsers][triton]") {
    std::array<std::uint8_t, 64> report{};
    REQUIRE(buildTritonConfigReport(SteamConfig::Restore, 0, report.data(), report.size()) == 64);
    checkSetting(report, kSettingImuMode, 0x0000); // SETTING_GYRO_MODE_OFF
    REQUIRE(buildTritonConfigReport(SteamConfig::Restore, 1, report.data(), report.size()) == 64);
    checkSetting(report, kSettingLizardMode, 1); // LIZARD_MODE_ON
    CHECK(buildTritonConfigReport(SteamConfig::Restore, 2, report.data(), report.size()) == 0);
}

TEST_CASE("a settings report refuses to overrun a caller buffer", "[usb-parsers][triton]") {
    std::array<std::uint8_t, 63> small{};
    CHECK(buildTritonConfigReport(SteamConfig::Quiet, 0, small.data(), small.size()) == 0);
    std::array<std::uint8_t, 64> report{};
    CHECK(buildTritonConfigReport(SteamConfig::Quiet, -1, report.data(), report.size()) == 0);
}

TEST_CASE("the settings a pad must keep receiving are the 2026 pad's quiet sequence",
          "[usb-parsers][triton]") {
    std::array<std::uint8_t, 64> keepalive{};
    std::array<std::uint8_t, 64> quiet{};
    for (int i = 0; i < 2; ++i) {
        REQUIRE(buildKeepaliveReport(HidParser::SteamTriton, i, keepalive.data(),
                                     keepalive.size()) == 64);
        REQUIRE(buildTritonConfigReport(SteamConfig::Quiet, i, quiet.data(), quiet.size()) == 64);
        CHECK(keepalive == quiet);
    }
    CHECK(buildKeepaliveReport(HidParser::SteamTriton, 2, keepalive.data(), keepalive.size()) == 0);
    // Every other family owes its pad nothing on a clock.
    for (const HidParser p :
         {HidParser::SteamController, HidParser::DualSense, HidParser::DualShock4,
          HidParser::SwitchProUsb, HidParser::GenericHid, HidParser::None}) {
        CHECK(buildKeepaliveReport(p, 0, keepalive.data(), keepalive.size()) == 0);
    }
}

// ── Rumble ──────────────────────────────────────────────────────────────────

TEST_CASE("host rumble is the 2026 pad's haptic rumble report", "[usb][output][triton]") {
    // OutputReportMsg: id 0x80, then MsgHapticRumble {type u8, intensity u16,
    // left {speed u16, gain i8}, right {speed u16, gain i8}}, 10 bytes. SDL puts
    // the low-frequency (strong) motor on the left and the high-frequency (weak)
    // one on the right, at full 16-bit resolution, with every other field zero.
    std::array<std::uint8_t, usbout::kMaxOutputReportBytes> buf{};
    const std::size_t n = usbout::buildRumbleReport(HidParser::SteamTriton, usbout::FeedbackState{},
                                                    /*strong=*/0xABCD, /*weak=*/0x1234,
                                                    /*seq=*/0, buf.data(), buf.size());
    REQUIRE(n == 10);
    const std::array<std::uint8_t, 10> expected{0x80, 0x00, 0x00, 0x00, 0xCD,
                                                0xAB, 0x00, 0x34, 0x12, 0x00};
    for (std::size_t i = 0; i < n; ++i) {
        INFO("byte " << i);
        CHECK(buf[i] == expected[i]);
    }
}

TEST_CASE("a 2026 rumble report needs its whole length", "[usb][output][triton]") {
    std::array<std::uint8_t, 9> tiny{};
    CHECK(usbout::buildRumbleReport(HidParser::SteamTriton, usbout::FeedbackState{}, 1, 1, 0,
                                    tiny.data(), tiny.size()) == 0);
}

TEST_CASE("the 2026 pad has no light bar or player LEDs to build for", "[usb][output][triton]") {
    std::array<std::uint8_t, usbout::kMaxOutputReportBytes> buf{};
    usbout::FeedbackState st;
    CHECK(usbout::buildLightbarReport(HidParser::SteamTriton, st, 1, 2, 3, buf.data(),
                                      buf.size()) == 0);
    CHECK(usbout::buildPlayerLedsReport(HidParser::SteamTriton, st, 1, 0, buf.data(), buf.size()) ==
          0);
}
