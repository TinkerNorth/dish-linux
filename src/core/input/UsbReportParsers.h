// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Per-model HID input-report decoders for the USB-direct path. Pure and
// allocation-free: they run on the gateway's read thread.
//
// Face buttons map by PHYSICAL POSITION, not label (Cross/bottom -> XUSB_A,
// Circle/right -> XUSB_B, Square/left -> XUSB_X, Triangle/top -> XUSB_Y), so
// games see the XInput layout.
//
// Button, stick and trigger offsets come from dish-android's hardware-validated
// usb_parsers.cpp and must stay in lockstep with it. The DS4/DualSense IMU and
// touchpad offsets do not: they come from the public hid-playstation layout,
// which android's USB path never exercises, so their sign and scale are
// unverified against real pads. Those sites are flagged inline.

#pragma once

#include "core/input/GamepadButtonLayouts.h"

#include <cstddef>
#include <cstdint>

namespace dish::input::usbparse {

// hidraw never sees Xbox or GIP pads on this path (xpad claims them as evdev),
// so only the HID-class families that can actually be claimed appear here.
enum class HidParser : std::uint8_t {
    None = 0,
    DualShock4,
    DualSense,
    SwitchProUsb,
    GenericHid,
    SteamController,
};

// Which direction a Steam Controller config sequence runs. Quiet is sent at
// attach; Restore must run on every exit path or the pad stays mute as a
// desktop mouse after we hand it back.
enum class SteamConfig : std::uint8_t {
    Quiet = 0,
    Restore = 1,
};

// Wireless dongle connect/disconnect events that arrive on the same endpoint as
// input reports. They never decode as state, so without them a departed pad
// would keep its last published input latched and a returning pad (fresh boot,
// settings gone) would stream without its quiet-mode init.
enum class WirelessEvent : std::uint8_t {
    None = 0,
    Connect = 1,
    Disconnect = 2,
};

enum class ButtonOrder : std::uint8_t {
    Western = 0,
    Switch = 1,
};

struct KnownHidModel {
    int vid;
    int pid;
    const char* name;
    HidParser parser;
    ButtonOrder order = ButtonOrder::Western;
};

// Expand-only stick auto-range for the Switch Pro: raw 12-bit ADC, asymmetric
// throw, and no factory calibration is read. One instance per claimed device,
// mutated in place each report so the decode stays allocation-free.
struct AxisAutoRange {
    std::int32_t posReach = 1000;
    std::int32_t negReach = 1000;
};

struct StickAutoRangeState {
    AxisAutoRange lx;
    AxisAutoRange ly;
    AxisAutoRange rx;
    AxisAutoRange ry;
    // Steam Controller stick, held across the frames where the shared left axes
    // carry pad data.
    std::int16_t steamStickX = 0;
    std::int16_t steamStickY = 0;
};

// Normalised to the XUSB axis/trigger scale.
struct ParsedReport {
    std::uint16_t wButtons = 0; // XUSB button bits (GamepadButtonLayouts kXusb*)
    std::uint8_t lt = 0;
    std::uint8_t rt = 0;
    std::int16_t lx = 0;
    std::int16_t ly = 0;
    std::int16_t rx = 0;
    std::int16_t ry = 0;

    // Wire int16 scale: gyro deg/s/2000*32767, accel g/4*32767.
    bool motionValid = false;
    std::int16_t gyroX = 0;
    std::int16_t gyroY = 0;
    std::int16_t gyroZ = 0;
    std::int16_t accelX = 0;
    std::int16_t accelY = 0;
    std::int16_t accelZ = 0;

    // Coordinates use the same signed int16 span as
    // SdlMotionConvert::touchpadCoordToInt16 (0 -> -32768, max -> +32767).
    bool touchpadValid = false;
    bool finger0Active = false;
    std::uint8_t finger0Id = 0;
    std::int16_t finger0X = 0;
    std::int16_t finger0Y = 0;
    bool finger1Active = false;
    std::uint8_t finger1Id = 0;
    std::int16_t finger1X = 0;
    std::int16_t finger1Y = 0;
    bool touchpadButton = false;
};

// `invert` flips the sense: PlayStation Y axes are down-positive on the wire,
// XUSB wants up-positive.
inline std::int16_t scaleU8Centered(std::uint8_t v, bool invert) {
    std::int32_t s =
        invert ? (128 - static_cast<std::int32_t>(v)) : (static_cast<std::int32_t>(v) - 128);
    std::int32_t scaled = s * 257;
    if (scaled > 32767) { scaled = 32767; }
    if (scaled < -32768) { scaled = -32768; }
    return static_cast<std::int16_t>(scaled);
}

// Raw 12-bit Switch domain. The Pro's centre wanders per unit and no factory
// calibration is read, so counts within the deadzone read as centre.
inline constexpr std::int32_t kSwitchStickRawDeadzone = 320;

// 12-bit Switch stick value, centred near 2048; each side auto-ranges separately.
inline std::int16_t scaleSwitchStickAuto(std::uint16_t raw12, AxisAutoRange& axis) {
    std::int32_t centered = static_cast<std::int32_t>(raw12) - 2048;
    std::int32_t mag = centered >= 0 ? centered : -centered;
    if (mag <= kSwitchStickRawDeadzone) { return 0; }
    std::int32_t adj = mag - kSwitchStickRawDeadzone;
    if (centered >= 0) {
        if (adj > axis.posReach) { axis.posReach = adj; }
        std::int32_t scaled = (adj * 32767) / axis.posReach;
        if (scaled > 32767) { scaled = 32767; }
        return static_cast<std::int16_t>(scaled);
    }
    if (adj > axis.negReach) { axis.negReach = adj; }
    std::int32_t scaled = (adj * 32768) / axis.negReach;
    if (scaled > 32768) { scaled = 32768; }
    return static_cast<std::int16_t>(-scaled);
}

inline std::int16_t rdLe16(const std::uint8_t* b, int off) {
    return static_cast<std::int16_t>(static_cast<std::uint16_t>(b[off]) |
                                     (static_cast<std::uint16_t>(b[off + 1]) << 8));
}

// Switch Pro IMU default scaling; no factory calibration is read.
inline std::int16_t switchGyroToWire(std::int16_t raw) {
    std::int64_t wire = static_cast<std::int64_t>(raw) * 32767 / 28568;
    if (wire > 32767) { wire = 32767; }
    if (wire < -32768) { wire = -32768; }
    return static_cast<std::int16_t>(wire);
}

inline std::int16_t switchAccelToWire(std::int16_t raw) {
    std::int64_t wire = static_cast<std::int64_t>(raw) * 32767 / 16384;
    if (wire > 32767) { wire = 32767; }
    if (wire < -32768) { wire = -32768; }
    return static_cast<std::int16_t>(wire);
}

// The 4-bit HID hat value runs 0..7 clockwise from North; >= 8 is neutral.
inline std::uint16_t setDpadFromHat(std::uint16_t buttons, std::uint8_t hat) {
    buttons =
        static_cast<std::uint16_t>(buttons & ~(layout::kXusbDpadUp | layout::kXusbDpadDown |
                                               layout::kXusbDpadLeft | layout::kXusbDpadRight));
    switch (hat & 0x0F) {
    case 0:
        buttons |= layout::kXusbDpadUp;
        break;
    case 1:
        buttons |= static_cast<std::uint16_t>(layout::kXusbDpadUp | layout::kXusbDpadRight);
        break;
    case 2:
        buttons |= layout::kXusbDpadRight;
        break;
    case 3:
        buttons |= static_cast<std::uint16_t>(layout::kXusbDpadDown | layout::kXusbDpadRight);
        break;
    case 4:
        buttons |= layout::kXusbDpadDown;
        break;
    case 5:
        buttons |= static_cast<std::uint16_t>(layout::kXusbDpadDown | layout::kXusbDpadLeft);
        break;
    case 6:
        buttons |= layout::kXusbDpadLeft;
        break;
    case 7:
        buttons |= static_cast<std::uint16_t>(layout::kXusbDpadUp | layout::kXusbDpadLeft);
        break;
    default:
        break;
    }
    return buttons;
}

// Integer math only, to stay FPU-free on the read thread.
inline std::int16_t touchAbsToInt16(std::int32_t value, std::int32_t maxValue) {
    if (maxValue <= 0) { return 0; }
    if (value < 0) { value = 0; }
    if (value > maxValue) { value = maxValue; }
    // Map [0, maxValue] -> [-32768, 32767] (range 65535).
    const std::int64_t span = 65535;
    std::int64_t scaled = (static_cast<std::int64_t>(value) * span) / maxValue - 32768;
    if (scaled > 32767) { scaled = 32767; }
    if (scaled < -32768) { scaled = -32768; }
    return static_cast<std::int16_t>(scaled);
}

// DualShock 4 USB report 0x01:
//   buf[0]=0x01 report id; buf[1..4]=LX/LY/RX/RY (uint8, 128 centre, Y inverted);
//   buf[5] low nibble=hat, bits: 0x10 Square(->X) 0x20 Cross(->A) 0x40 Circle(->B)
//   0x80 Triangle(->Y); buf[6]: 0x01 L1 0x02 R1 0x10 Share(Back) 0x20 Options(Start)
//   0x40 L3 0x80 R3; buf[8]=L2 analog buf[9]=R2 analog.
inline bool decodeDualShock4(const std::uint8_t* buf, std::size_t len, ParsedReport& s,
                             StickAutoRangeState& /*unused*/) {
    if (len < 10) { return false; }
    if (buf[0] != 0x01) { return false; }

    s.lx = scaleU8Centered(buf[1], false);
    s.ly = scaleU8Centered(buf[2], true);
    s.rx = scaleU8Centered(buf[3], false);
    s.ry = scaleU8Centered(buf[4], true);

    std::uint16_t b = 0;
    if ((buf[5] & 0x10) != 0) { b |= layout::kXusbX; }
    if ((buf[5] & 0x20) != 0) { b |= layout::kXusbA; }
    if ((buf[5] & 0x40) != 0) { b |= layout::kXusbB; }
    if ((buf[5] & 0x80) != 0) { b |= layout::kXusbY; }
    if ((buf[6] & 0x01) != 0) { b |= layout::kXusbLeftShoulder; }
    if ((buf[6] & 0x02) != 0) { b |= layout::kXusbRightShoulder; }
    if ((buf[6] & 0x10) != 0) { b |= layout::kXusbBack; }
    if ((buf[6] & 0x20) != 0) { b |= layout::kXusbStart; }
    if ((buf[6] & 0x40) != 0) { b |= layout::kXusbLeftThumb; }
    if ((buf[6] & 0x80) != 0) { b |= layout::kXusbRightThumb; }
    // PS/Guide button.
    if (len > 7 && (buf[7] & 0x01) != 0) { b |= layout::kXusbGuide; }
    b = setDpadFromHat(b, static_cast<std::uint8_t>(buf[5] & 0x0F));
    s.wButtons = b;

    s.lt = buf[8];
    s.rt = buf[9];

    // Gyro int16 LE at buf[13/15/17], accel at buf[19/21/23]. UNVALIDATED: the
    // Switch wire-scale factors are a placeholder, and the DS4 LSB differs.
    if (len >= 25) {
        s.gyroX = switchGyroToWire(rdLe16(buf, 13));
        s.gyroY = switchGyroToWire(rdLe16(buf, 15));
        s.gyroZ = switchGyroToWire(rdLe16(buf, 17));
        s.accelX = switchAccelToWire(rdLe16(buf, 19));
        s.accelY = switchAccelToWire(rdLe16(buf, 21));
        s.accelZ = switchAccelToWire(rdLe16(buf, 23));
        s.motionValid = true;
    }

    // UNVALIDATED. Touch block at buf[35]; 4 bytes per finger: [0] = id | active
    // (bit7 clear == touching), [1] = x low 8, [2] = (y low nibble << 4) | x high
    // nibble, [3] = y high 8. Finger 1 at buf[39]. The pad is 1920x942.
    if (len >= 43) {
        const std::uint8_t* t0 = buf + 35;
        const std::uint8_t* t1 = buf + 39;
        const bool f0Touch = (t0[0] & 0x80) == 0;
        const bool f1Touch = (t1[0] & 0x80) == 0;
        s.finger0Active = f0Touch;
        s.finger0Id = static_cast<std::uint8_t>(t0[0] & 0x7F);
        s.finger0X = touchAbsToInt16(static_cast<std::int32_t>(t0[1]) |
                                         ((static_cast<std::int32_t>(t0[2]) & 0x0F) << 8),
                                     1919);
        s.finger0Y = touchAbsToInt16(
            (static_cast<std::int32_t>(t0[2]) >> 4) | (static_cast<std::int32_t>(t0[3]) << 4), 941);
        s.finger1Active = f1Touch;
        s.finger1Id = static_cast<std::uint8_t>(t1[0] & 0x7F);
        s.finger1X = touchAbsToInt16(static_cast<std::int32_t>(t1[1]) |
                                         ((static_cast<std::int32_t>(t1[2]) & 0x0F) << 8),
                                     1919);
        s.finger1Y = touchAbsToInt16(
            (static_cast<std::int32_t>(t1[2]) >> 4) | (static_cast<std::int32_t>(t1[3]) << 4), 941);
        // The clickable pad is buf[7] bit 0x02.
        s.touchpadButton = len > 7 && (buf[7] & 0x02) != 0;
        s.touchpadValid = true;
    }
    return true;
}

// DualSense USB report 0x01:
//   buf[0]=0x01; buf[1..4]=LX/LY/RX/RY (uint8, 128 centre, Y inverted); buf[5]=L2
//   buf[6]=R2 analog; buf[8] face/hat (low nibble hat, 0x10 Square/X 0x20 Cross/A
//   0x40 Circle/B 0x80 Triangle/Y); buf[9]: 0x01 L1 0x02 R1 0x10 Create(Back) 0x20
//   Options(Start) 0x40 L3 0x80 R3.
inline bool decodeDualSense(const std::uint8_t* buf, std::size_t len, ParsedReport& s,
                            StickAutoRangeState& /*unused*/) {
    if (len < 11) { return false; }
    if (buf[0] != 0x01) { return false; }

    s.lx = scaleU8Centered(buf[1], false);
    s.ly = scaleU8Centered(buf[2], true);
    s.rx = scaleU8Centered(buf[3], false);
    s.ry = scaleU8Centered(buf[4], true);

    s.lt = buf[5];
    s.rt = buf[6];

    std::uint16_t b = 0;
    if ((buf[8] & 0x10) != 0) { b |= layout::kXusbX; }
    if ((buf[8] & 0x20) != 0) { b |= layout::kXusbA; }
    if ((buf[8] & 0x40) != 0) { b |= layout::kXusbB; }
    if ((buf[8] & 0x80) != 0) { b |= layout::kXusbY; }
    if ((buf[9] & 0x01) != 0) { b |= layout::kXusbLeftShoulder; }
    if ((buf[9] & 0x02) != 0) { b |= layout::kXusbRightShoulder; }
    if ((buf[9] & 0x10) != 0) { b |= layout::kXusbBack; }
    if ((buf[9] & 0x20) != 0) { b |= layout::kXusbStart; }
    if ((buf[9] & 0x40) != 0) { b |= layout::kXusbLeftThumb; }
    if ((buf[9] & 0x80) != 0) { b |= layout::kXusbRightThumb; }
    // PS button.
    if (len > 10 && (buf[10] & 0x01) != 0) { b |= layout::kXusbGuide; }
    b = setDpadFromHat(b, static_cast<std::uint8_t>(buf[8] & 0x0F));
    s.wButtons = b;

    // Gyro int16 LE at buf[16/18/20], accel at buf[22/24/26]. UNVALIDATED, and the
    // wire-scale factors are the same DS4 placeholder.
    if (len >= 28) {
        s.gyroX = switchGyroToWire(rdLe16(buf, 16));
        s.gyroY = switchGyroToWire(rdLe16(buf, 18));
        s.gyroZ = switchGyroToWire(rdLe16(buf, 20));
        s.accelX = switchAccelToWire(rdLe16(buf, 22));
        s.accelY = switchAccelToWire(rdLe16(buf, 24));
        s.accelZ = switchAccelToWire(rdLe16(buf, 26));
        s.motionValid = true;
    }

    // UNVALIDATED. Touch block at buf[33], same 4-byte-per-finger packing as the
    // DS4. The pad is 1920x1080.
    if (len >= 41) {
        const std::uint8_t* t0 = buf + 33;
        const std::uint8_t* t1 = buf + 37;
        s.finger0Active = (t0[0] & 0x80) == 0;
        s.finger0Id = static_cast<std::uint8_t>(t0[0] & 0x7F);
        s.finger0X = touchAbsToInt16(static_cast<std::int32_t>(t0[1]) |
                                         ((static_cast<std::int32_t>(t0[2]) & 0x0F) << 8),
                                     1919);
        s.finger0Y = touchAbsToInt16((static_cast<std::int32_t>(t0[2]) >> 4) |
                                         (static_cast<std::int32_t>(t0[3]) << 4),
                                     1079);
        s.finger1Active = (t1[0] & 0x80) == 0;
        s.finger1Id = static_cast<std::uint8_t>(t1[0] & 0x7F);
        s.finger1X = touchAbsToInt16(static_cast<std::int32_t>(t1[1]) |
                                         ((static_cast<std::int32_t>(t1[2]) & 0x0F) << 8),
                                     1919);
        s.finger1Y = touchAbsToInt16((static_cast<std::int32_t>(t1[2]) >> 4) |
                                         (static_cast<std::int32_t>(t1[3]) << 4),
                                     1079);
        // Pad-click.
        s.touchpadButton = len > 10 && (buf[10] & 0x02) != 0;
        s.touchpadValid = true;
    }
    return true;
}

// Switch Pro standard full input report 0x30 over USB:
//   buf[0]=0x30; buf[3]=right buttons, buf[4]=shared, buf[5]=left; sticks packed
//   12-bit at buf[6..11]; ZL/ZR digital; IMU frames from buf[13].
inline bool decodeSwitchProUsb(const std::uint8_t* buf, std::size_t len, ParsedReport& s,
                               StickAutoRangeState& sticks) {
    if (len < 12) { return false; }
    if (buf[0] != 0x30) { return false; }

    const std::uint8_t br = buf[3];
    const std::uint8_t bs = buf[4];
    const std::uint8_t bl = buf[5];

    std::uint16_t b = 0;
    if ((br & 0x01) != 0) { b |= layout::kXusbX; }
    if ((br & 0x02) != 0) { b |= layout::kXusbY; }
    if ((br & 0x04) != 0) { b |= layout::kXusbA; }
    if ((br & 0x08) != 0) { b |= layout::kXusbB; }
    if ((br & 0x40) != 0) { b |= layout::kXusbRightShoulder; }
    if ((bs & 0x01) != 0) { b |= layout::kXusbBack; }
    if ((bs & 0x02) != 0) { b |= layout::kXusbStart; }
    if ((bs & 0x04) != 0) { b |= layout::kXusbRightThumb; }
    if ((bs & 0x08) != 0) { b |= layout::kXusbLeftThumb; }
    if ((bs & 0x10) != 0) { b |= layout::kXusbGuide; } // Home
    if ((bl & 0x01) != 0) { b |= layout::kXusbDpadDown; }
    if ((bl & 0x02) != 0) { b |= layout::kXusbDpadUp; }
    if ((bl & 0x04) != 0) { b |= layout::kXusbDpadRight; }
    if ((bl & 0x08) != 0) { b |= layout::kXusbDpadLeft; }
    if ((bl & 0x40) != 0) { b |= layout::kXusbLeftShoulder; }
    s.wButtons = b;

    // ZL/ZR are digital on the Pro.
    s.lt = (bl & 0x80) != 0 ? 255 : 0;
    s.rt = (br & 0x80) != 0 ? 255 : 0;

    // 12-bit fields packed across byte boundaries. The shifts promote to int,
    // so each result is narrowed back explicitly; every value fits 12 bits, so
    // no bit is lost.
    const auto lx = static_cast<std::uint16_t>(static_cast<std::uint16_t>(buf[6]) |
                                               ((static_cast<std::uint16_t>(buf[7]) & 0x0F) << 8));
    const auto ly = static_cast<std::uint16_t>((static_cast<std::uint16_t>(buf[7]) >> 4) |
                                               (static_cast<std::uint16_t>(buf[8]) << 4));
    const auto rx = static_cast<std::uint16_t>(static_cast<std::uint16_t>(buf[9]) |
                                               ((static_cast<std::uint16_t>(buf[10]) & 0x0F) << 8));
    const auto ry = static_cast<std::uint16_t>((static_cast<std::uint16_t>(buf[10]) >> 4) |
                                               (static_cast<std::uint16_t>(buf[11]) << 4));

    s.lx = scaleSwitchStickAuto(lx, sticks.lx);
    s.ly = scaleSwitchStickAuto(ly, sticks.ly);
    s.rx = scaleSwitchStickAuto(rx, sticks.rx);
    s.ry = scaleSwitchStickAuto(ry, sticks.ry);

    // Three 12-byte IMU frames start at byte 13 (accel int16 LE at +0/+2/+4, gyro
    // at +6/+8/+10); only the first is used. Signs may need an on-device flip.
    if (len >= 25) {
        s.accelX = switchAccelToWire(rdLe16(buf, 13));
        s.accelY = switchAccelToWire(rdLe16(buf, 15));
        s.accelZ = switchAccelToWire(rdLe16(buf, 17));
        s.gyroX = switchGyroToWire(rdLe16(buf, 19));
        s.gyroY = switchGyroToWire(rdLe16(buf, 21));
        s.gyroZ = switchGyroToWire(rdLe16(buf, 23));
        s.motionValid = true;
    }
    return true;
}

// Best-effort for unknown pads: 4 axes at buf[0..3] (uint8 centred, Y inverted),
// hat in buf[4] low nibble, face buttons in buf[4] high nibble,
// shoulders/thumbs/triggers in buf[5]. The length check keeps non-gamepad reports out.
inline bool decodeGenericHid(const std::uint8_t* buf, std::size_t len, ParsedReport& s,
                             StickAutoRangeState& /*unused*/) {
    if (len < 7) { return false; }
    s.lx = scaleU8Centered(buf[0], false);
    s.ly = scaleU8Centered(buf[1], true);
    s.rx = scaleU8Centered(buf[2], false);
    s.ry = scaleU8Centered(buf[3], true);
    std::uint16_t b = 0;
    b = setDpadFromHat(b, static_cast<std::uint8_t>(buf[4] & 0x0F));
    const std::uint8_t btnLo = buf[4];
    const std::uint8_t btnHi = len > 5 ? buf[5] : 0;
    if ((btnLo & 0x10) != 0) { b |= layout::kXusbA; }
    if ((btnLo & 0x20) != 0) { b |= layout::kXusbB; }
    if ((btnLo & 0x40) != 0) { b |= layout::kXusbX; }
    if ((btnLo & 0x80) != 0) { b |= layout::kXusbY; }
    if ((btnHi & 0x01) != 0) { b |= layout::kXusbLeftShoulder; }
    if ((btnHi & 0x02) != 0) { b |= layout::kXusbRightShoulder; }
    if ((btnHi & 0x04) != 0) { b |= layout::kXusbBack; }
    if ((btnHi & 0x08) != 0) { b |= layout::kXusbStart; }
    if ((btnHi & 0x10) != 0) { b |= layout::kXusbLeftThumb; }
    if ((btnHi & 0x20) != 0) { b |= layout::kXusbRightThumb; }
    s.wButtons = b;
    s.lt = (btnHi & 0x40) != 0 ? 255 : 0;
    s.rt = (btnHi & 0x80) != 0 ? 255 : 0;
    return true;
}

// ── Steam Controller ────────────────────────────────────────────────────────
// The 64-byte vendor-interface packets, quiet/restore config sequences and the
// dongle's wireless events mirror dish-android's hardware-validated port of the
// Linux hid-steam / SDL drivers, byte for byte.

inline constexpr std::uint32_t kSteamRightBumper = 0x000004;
inline constexpr std::uint32_t kSteamLeftBumper = 0x000008;
inline constexpr std::uint32_t kSteamNorth = 0x000010;
inline constexpr std::uint32_t kSteamEast = 0x000020;
inline constexpr std::uint32_t kSteamWest = 0x000040;
inline constexpr std::uint32_t kSteamSouth = 0x000080;
inline constexpr std::uint32_t kSteamDpadUp = 0x000100;
inline constexpr std::uint32_t kSteamDpadRight = 0x000200;
inline constexpr std::uint32_t kSteamDpadLeft = 0x000400;
inline constexpr std::uint32_t kSteamDpadDown = 0x000800;
inline constexpr std::uint32_t kSteamMenu = 0x001000;
inline constexpr std::uint32_t kSteamGuide = 0x002000;
inline constexpr std::uint32_t kSteamEscape = 0x004000;
inline constexpr std::uint32_t kSteamLeftPadClicked = 0x020000;
inline constexpr std::uint32_t kSteamRightPadClicked = 0x040000;
inline constexpr std::uint32_t kSteamLeftPadFinger = 0x080000;
inline constexpr std::uint32_t kSteamRightPadFinger = 0x100000;
inline constexpr std::uint32_t kSteamStickButton = 0x400000;
inline constexpr std::uint32_t kSteamLeftPadAndStick = 0x800000;

inline constexpr std::size_t kSteamStateLen = 48;
inline constexpr std::uint8_t kSteamStateType = 0x01;
inline constexpr std::uint8_t kSteamWirelessType = 0x03;
inline constexpr std::uint8_t kSteamWirelessDisconnect = 0x01;
inline constexpr std::uint8_t kSteamWirelessConnect = 0x02;
inline constexpr std::int32_t kSteamTriggerMaxAnalog = 26000;
// 15 degrees in Q16; the pads sit rotated on the shell and SDL applies the same
// correction.
inline constexpr std::int32_t kSteamPadCos = 63303;
inline constexpr std::int32_t kSteamPadSin = 16962;

inline std::int16_t steamClampI16(std::int64_t v) {
    if (v > 32767) { return 32767; }
    if (v < -32768) { return -32768; }
    return static_cast<std::int16_t>(v);
}

// Valve's 26000 full scale, so the throw saturates before the raw rail.
inline std::uint8_t steamTriggerToWire(std::uint8_t raw) {
    std::int32_t v = (static_cast<std::int32_t>(raw) << 7) | raw;
    if (v > kSteamTriggerMaxAnalog) { v = kSteamTriggerMaxAnalog; }
    return static_cast<std::uint8_t>((v * 255) / kSteamTriggerMaxAnalog);
}

// SDL adds a further +1000 to each axis while a finger is down. That is
// harmless for a touch surface but would park a stick off centre, so it is
// deliberately not carried over.
inline void steamRotatePad(std::int32_t x, std::int32_t y, std::int16_t& outX, std::int16_t& outY) {
    outX = steamClampI16((static_cast<std::int64_t>(kSteamPadCos) * x -
                          static_cast<std::int64_t>(kSteamPadSin) * y) /
                         65536);
    outY = steamClampI16((static_cast<std::int64_t>(kSteamPadSin) * x +
                          static_cast<std::int64_t>(kSteamPadCos) * y) /
                         65536);
}

// Steam reports gyro over +-2000 deg/s and accel over +-2g; the wire wants
// deg/s / 2000 and g / 4.
inline std::int16_t steamGyroToWire(std::int32_t raw) {
    return steamClampI16(static_cast<std::int64_t>(raw) * 32767 / 32768);
}

inline std::int16_t steamAccelToWire(std::int32_t raw) {
    return steamClampI16(static_cast<std::int64_t>(raw) * 32767 / 65536);
}

inline bool decodeSteamController(const std::uint8_t* buf, std::size_t len, ParsedReport& s,
                                  StickAutoRangeState& st) {
    if (len < kSteamStateLen) { return false; }
    if (buf[0] != 0x01 || buf[1] != 0x00) { return false; }
    if (buf[2] != kSteamStateType) { return false; }

    const std::uint32_t btn = static_cast<std::uint32_t>(buf[8]) |
                              (static_cast<std::uint32_t>(buf[9]) << 8) |
                              (static_cast<std::uint32_t>(buf[10]) << 16);

    std::uint16_t b = 0;
    if ((btn & kSteamSouth) != 0) { b |= layout::kXusbA; }
    if ((btn & kSteamEast) != 0) { b |= layout::kXusbB; }
    if ((btn & kSteamWest) != 0) { b |= layout::kXusbX; }
    if ((btn & kSteamNorth) != 0) { b |= layout::kXusbY; }
    if ((btn & kSteamLeftBumper) != 0) { b |= layout::kXusbLeftShoulder; }
    if ((btn & kSteamRightBumper) != 0) { b |= layout::kXusbRightShoulder; }
    if ((btn & kSteamMenu) != 0) { b |= layout::kXusbBack; }
    if ((btn & kSteamEscape) != 0) { b |= layout::kXusbStart; }
    if ((btn & kSteamGuide) != 0) { b |= layout::kXusbGuide; }
    if ((btn & kSteamDpadUp) != 0) { b |= layout::kXusbDpadUp; }
    if ((btn & kSteamDpadDown) != 0) { b |= layout::kXusbDpadDown; }
    if ((btn & kSteamDpadLeft) != 0) { b |= layout::kXusbDpadLeft; }
    if ((btn & kSteamDpadRight) != 0) { b |= layout::kXusbDpadRight; }
    if ((btn & kSteamRightPadClicked) != 0) { b |= layout::kXusbRightThumb; }

    s.lt = steamTriggerToWire(buf[11]);
    s.rt = steamTriggerToWire(buf[12]);

    // One pair of axes carries either the stick or the left pad. The
    // finger-down bit says which; the interleave bit means pad frames alternate
    // with stick frames worth holding on to.
    const bool padOnLeft = (btn & kSteamLeftPadFinger) != 0;
    const bool interleaved = (btn & kSteamLeftPadAndStick) != 0;
    if (!padOnLeft) {
        st.steamStickX = rdLe16(buf, 16);
        st.steamStickY = rdLe16(buf, 18);
        // With no live pad the firmware reports a stick click as a left-pad
        // click.
        if (!interleaved && (btn & kSteamLeftPadClicked) != 0) { b |= layout::kXusbLeftThumb; }
    } else if (!interleaved) {
        st.steamStickX = 0;
        st.steamStickY = 0;
    }
    if ((btn & kSteamStickButton) != 0) { b |= layout::kXusbLeftThumb; }
    s.wButtons = b;
    s.lx = st.steamStickX;
    s.ly = st.steamStickY;

    if ((btn & kSteamRightPadFinger) != 0) {
        steamRotatePad(rdLe16(buf, 20), rdLe16(buf, 22), s.rx, s.ry);
    } else {
        s.rx = 0;
        s.ry = 0;
    }

    const std::int16_t ax = rdLe16(buf, 28);
    const std::int16_t ay = rdLe16(buf, 30);
    const std::int16_t az = rdLe16(buf, 32);
    const std::int16_t gx = rdLe16(buf, 34);
    const std::int16_t gy = rdLe16(buf, 36);
    const std::int16_t gz = rdLe16(buf, 38);
    // An all-zero block means the IMU setting never took; publishing it would
    // stream a dead sensor.
    if ((ax | ay | az | gx | gy | gz) != 0) {
        s.gyroX = steamGyroToWire(gx);
        s.gyroY = steamGyroToWire(gz);
        s.gyroZ = steamGyroToWire(gy);
        s.accelX = steamAccelToWire(ax);
        s.accelY = steamAccelToWire(az);
        s.accelZ = steamAccelToWire(-static_cast<std::int32_t>(ay));
        s.motionValid = true;
    }
    return true;
}

// Pure: writes the index-th Steam Controller feature report for a config
// direction into out, returns its length or 0 when there are no more. Payload
// only; the caller frames it as a feature report.
inline std::size_t buildSteamConfigPacket(SteamConfig stage, int index, std::uint8_t* out,
                                          std::size_t outCap) {
    // Framing is {message id, payload length, payload}. Message ids and the
    // choice of the shortest working sequence follow the Linux hid-steam driver.
    static constexpr std::uint8_t kClearMappings[] = {0x81, 0x00};
    // Left and right trackpad mode = none (kills mouse emulation), IMU mode =
    // raw accel | raw gyro.
    static constexpr std::uint8_t kQuietSettings[] = {0x87, 0x09, 0x07, 0x07, 0x00, 0x08,
                                                      0x07, 0x00, 0x30, 0x18, 0x00};
    static constexpr std::uint8_t kDefaultMappings[] = {0x85, 0x00};
    static constexpr std::uint8_t kDefaultSettings[] = {0x8E, 0x00};
    // Loading the defaults does not by itself hand the right pad back as a
    // mouse: SDL follows it with an explicit right trackpad mode = absolute
    // mouse, and leaving that out is the one way this teardown could still
    // return a pad its owner cannot use.
    static constexpr std::uint8_t kRestoreMouse[] = {0x87, 0x03, 0x08, 0x00, 0x00};

    struct Pkt {
        const std::uint8_t* data;
        std::size_t len;
    };
    static constexpr Pkt kQuietSeq[] = {{kClearMappings, sizeof(kClearMappings)},
                                        {kQuietSettings, sizeof(kQuietSettings)}};
    static constexpr Pkt kRestoreSeq[] = {{kDefaultMappings, sizeof(kDefaultMappings)},
                                          {kDefaultSettings, sizeof(kDefaultSettings)},
                                          {kRestoreMouse, sizeof(kRestoreMouse)}};

    const bool quiet = stage == SteamConfig::Quiet;
    const Pkt* seqArr = quiet ? kQuietSeq : kRestoreSeq;
    const int seqLen = quiet ? static_cast<int>(sizeof(kQuietSeq) / sizeof(kQuietSeq[0]))
                             : static_cast<int>(sizeof(kRestoreSeq) / sizeof(kRestoreSeq[0]));
    if (index < 0 || index >= seqLen) { return 0; }
    const std::size_t len = seqArr[index].len;
    if (len > outCap) { return 0; }
    for (std::size_t i = 0; i < len; i++) { out[i] = seqArr[index].data[i]; }
    return len;
}

// Pure: classifies a report as a wireless connect/disconnect event for families
// that interleave them with input (the Steam Controller dongle). None for every
// other parser and packet. Event framing and the one-byte payload values follow
// the Linux hid-steam driver (steam_raw_event, ID_CONTROLLER_WIRELESS).
inline WirelessEvent checkWirelessEvent(HidParser p, const std::uint8_t* buf, std::size_t len) {
    if (p != HidParser::SteamController) { return WirelessEvent::None; }
    if (len < 5) { return WirelessEvent::None; }
    if (buf[0] != 0x01 || buf[1] != 0x00) { return WirelessEvent::None; }
    if (buf[2] != kSteamWirelessType || buf[3] != 0x01) { return WirelessEvent::None; }
    switch (buf[4]) {
    case kSteamWirelessDisconnect:
        return WirelessEvent::Disconnect;
    case kSteamWirelessConnect:
        return WirelessEvent::Connect;
    default:
        return WirelessEvent::None;
    }
}

// False leaves `out` untouched: the family is None, or the report was too short
// or carried the wrong id.
inline bool decodeReport(HidParser parser, const std::uint8_t* buf, std::size_t len,
                         ParsedReport& out, StickAutoRangeState& sticks) {
    switch (parser) {
    case HidParser::DualShock4:
        return decodeDualShock4(buf, len, out, sticks);
    case HidParser::DualSense:
        return decodeDualSense(buf, len, out, sticks);
    case HidParser::SwitchProUsb:
        return decodeSwitchProUsb(buf, len, out, sticks);
    case HidParser::GenericHid:
        return decodeGenericHid(buf, len, out, sticks);
    case HidParser::SteamController:
        return decodeSteamController(buf, len, out, sticks);
    case HidParser::None:
        return false;
    }
    return false;
}

// Models the vendor heuristic below cannot place, imported from dish-android's
// hardware-validated catalog. The PDP wired Switch pads declare their buttons
// in the Switch usage order (0x0186 Afterglow Wireless is excluded on purpose:
// its USB port is charge-only and it speaks the Switch Pro protocol, so the
// wired Switch-order remap would be wrong for it). None of these rows is on the
// verified fast lane, so nothing here auto-claims.
inline constexpr KnownHidModel kKnownModels[] = {
    {0x0E6F, 0x0180, "PDP Faceoff Wired Pro Controller (Switch)", HidParser::GenericHid,
     ButtonOrder::Switch},
    {0x0E6F, 0x0181, "PDP Faceoff Deluxe Wired Pro Controller (Switch)", HidParser::GenericHid,
     ButtonOrder::Switch},
    {0x0E6F, 0x0184, "PDP Faceoff Deluxe+ Audio Controller (Switch)", HidParser::GenericHid,
     ButtonOrder::Switch},
    {0x0E6F, 0x0185, "PDP Wired Fight Pad Pro (Switch)", HidParser::GenericHid,
     ButtonOrder::Switch},
    {0x0E6F, 0x0187, "PDP Rock Candy Wired Controller (Switch)", HidParser::GenericHid,
     ButtonOrder::Switch},
    {0x28DE, 0x1102, "Valve Steam Controller", HidParser::SteamController},
    {0x28DE, 0x1142, "Valve Steam Controller (dongle)", HidParser::SteamController},
};

inline const KnownHidModel* lookupKnownModel(int vendorId, int productId) {
    for (const auto& m : kKnownModels) {
        if (m.vid == vendorId && m.pid == productId) { return &m; }
    }
    return nullptr;
}

// Every non-DualSense Sony pad is assumed to speak the DS4 layout.
inline HidParser parserForDevice(int vendorId, int productId) {
    constexpr int kVidSony = 0x054C;
    constexpr int kVidNintendo = 0x057E;
    constexpr int kPidDualSense = 0x0CE6;
    constexpr int kPidDualSenseEdge = 0x0DF2;
    if (const KnownHidModel* m = lookupKnownModel(vendorId, productId)) { return m->parser; }
    if (vendorId == kVidSony) {
        if (productId == kPidDualSense || productId == kPidDualSenseEdge) {
            return HidParser::DualSense;
        }
        return HidParser::DualShock4;
    }
    if (vendorId == kVidNintendo) { return HidParser::SwitchProUsb; }
    return HidParser::GenericHid;
}

inline ButtonOrder buttonOrderForDevice(int vendorId, int productId) {
    const KnownHidModel* m = lookupKnownModel(vendorId, productId);
    return m != nullptr ? m->order : ButtonOrder::Western;
}

// Whether releasing this model back to Standard produces an SDL device.
// False for the Steam Controller, whose stand-alone identity is a keyboard and
// mouse: a release that waited for a framework gamepad would always time out
// into a false "restore stuck".
inline bool modelExpectsFrameworkGamepad(int vendorId, int productId) {
    const KnownHidModel* m = lookupKnownModel(vendorId, productId);
    return m == nullptr || m->parser != HidParser::SteamController;
}

inline bool parserHasImu(HidParser parser) {
    return parser == HidParser::DualShock4 || parser == HidParser::DualSense ||
           parser == HidParser::SwitchProUsb || parser == HidParser::SteamController;
}

inline bool parserHasTouchpad(HidParser parser) {
    return parser == HidParser::DualShock4 || parser == HidParser::DualSense;
}

// Whether the pad itself carries rumble motors this path could ever drive. The
// Steam Controller has no rumble motors, only trackpad voice coils driven by
// pulse trains; its simple-rumble command is Steam Deck firmware only.
inline bool parserHasRumble(HidParser parser) {
    return parser == HidParser::DualShock4 || parser == HidParser::DualSense ||
           parser == HidParser::SwitchProUsb;
}

} // namespace dish::input::usbparse
