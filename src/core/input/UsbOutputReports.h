// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Pure builders for the OUT reports a Direct-claimed pad accepts: rumble, the
// RGB lightbar, the player-indicator LEDs and the DualSense adaptive-trigger
// effect blocks. The mirror image of UsbReportParsers.h, which decodes the IN
// direction.
//
// Why a separate header and not a gateway method: the bytes are a protocol, not
// IO. Building them here keeps every offset under host unit test (there is no
// way to assert a firmware write) and leaves the platform gateway with the one
// thing it alone can do, hand the buffer to the device.
//
// Buffers are the FULL transfer including the leading report id, matching what
// both platforms want: a hidraw write() takes the id as byte 0, and the Windows
// HID stack takes the id as byte 0 of a buffer padded to OutputReportByteLength
// (the padding is the gateway's job, not this file's, because it is a
// per-device property rather than a per-report one).
//
// Every builder returns bytes written, and 0 for a family without the surface,
// so an unsupported pad is a no-op rather than a malformed write. Sources: the
// PS4/PS5/Switch HIDAPI drivers in SDL and the kernel's hid-playstation and
// hid-nintendo, cross-checked against the byte layouts dish-android already
// ships against real hardware.

#pragma once

#include "core/input/UsbReportParsers.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace dish::input::usbout {

using dish::input::usbparse::HidParser;

// The largest OUT report any supported family takes (DualSense USB, report
// 0x02). Callers size their scratch buffer with this.
inline constexpr std::size_t kMaxOutputReportBytes = 63;

// A DualSense trigger-effect block: the mode byte plus ten parameters. Equal to
// proto::kTriggerEffectBlockBytes, restated here so this header stays free of
// the wire protocol. These are hardware fields the protocol happens to carry,
// not protocol fields the hardware happens to take.
inline constexpr std::size_t kTriggerEffectBlockBytes = 11;

// -- Which families carry which actuator ------------------------------------
// Only the Direct claim path reaches any of these. SDL exposes rumble and a
// single LED colour and nothing else, and neither platform's framework layer
// has a player-LED or trigger-effect API at all.

inline bool parserHasLightbar(HidParser p) {
    return p == HidParser::DualShock4 || p == HidParser::DualSense;
}

// The DualSense drives a 5-LED bar under the touchpad, the Switch Pro 4 player
// lights along the grip.
inline bool parserHasPlayerLeds(HidParser p) {
    return p == HidParser::DualSense || p == HidParser::SwitchProUsb;
}

inline bool parserHasTriggerEffects(HidParser p) { return p == HidParser::DualSense; }

// Merged state the writer owns across calls. The DualSense lightbar needs a
// one-time handoff before a host colour is visible at all, which is a property
// of the connection rather than of any one write.
//
// There is deliberately no trigger-motor state here, unlike dish-android: the
// only family with impulse triggers is the Xbox One/Series GIP pad, and neither
// platform's Direct path can reach one (Linux xpad publishes no hidraw node for
// it; Windows XInput hides it from raw HID).
struct FeedbackState {
    bool ds5LightbarSetupSent = false;
};

namespace detail {

// Switch Pro HD-rumble amplitude codes. The motors take an encoded (high, low)
// pair rather than a magnitude, so a linear scale has to be quantised onto this
// table: the same eight steps hid-nintendo and SDL use.
struct SwitchAmpCode {
    std::uint8_t high;
    std::uint16_t low;
    std::uint16_t amp;
};

inline constexpr SwitchAmpCode kSwitchAmpCodes[] = {
    {0x00, 0x0040, 0},   {0x02, 0x8040, 10},  {0x08, 0x0042, 17},  {0x10, 0x0044, 33},
    {0x40, 0x0050, 230}, {0x70, 0x005C, 387}, {0xA0, 0x0068, 650}, {0xC8, 0x0072, 1003},
};

// Writes the 4-byte encoded amplitude for one motor. Picks the highest code at
// or below the requested amplitude, so a magnitude between steps rounds DOWN
// and a zero magnitude lands exactly on the neutral code.
inline void switchEncodeMotor(std::uint8_t* out, std::uint16_t magnitude) {
    const std::uint32_t amp = static_cast<std::uint32_t>(magnitude) * 1003U / 65535U;
    const SwitchAmpCode* code = &kSwitchAmpCodes[0];
    for (const auto& e : kSwitchAmpCodes) {
        if (e.amp <= amp) {
            code = &e;
        } else {
            break;
        }
    }
    out[0] = 0x00;
    out[1] = static_cast<std::uint8_t>(0x01U + code->high);
    out[2] = static_cast<std::uint8_t>(0x40U + ((code->low >> 8) & 0xFFU));
    out[3] = static_cast<std::uint8_t>(code->low & 0xFFU);
}

inline void zero(std::uint8_t* out, std::size_t n) { std::memset(out, 0, n); }

// The high byte of a 16-bit magnitude: DS4 and DualSense motors are 8-bit.
inline std::uint8_t hi8(std::uint16_t v) { return static_cast<std::uint8_t>(v >> 8); }

} // namespace detail

// Rumble. `seq` is a rolling counter the Switch Pro requires in the low nibble
// of every packet it accepts; the PlayStation families ignore it.
inline std::size_t buildRumbleReport(HidParser p, std::uint16_t strong, std::uint16_t weak,
                                     std::uint8_t seq, std::uint8_t* out, std::size_t outCap) {
    switch (p) {
    case HidParser::DualShock4:
        if (outCap < 32) { return 0; }
        detail::zero(out, 32);
        out[0] = 0x05;
        out[1] = 0x01; // valid flags: motors only
        out[4] = detail::hi8(weak);
        out[5] = detail::hi8(strong);
        return 32;
    case HidParser::DualSense:
        if (outCap < 63) { return 0; }
        detail::zero(out, 63);
        out[0] = 0x02;
        out[1] = 0x01; // valid_flag0 COMPATIBLE_VIBRATION
        out[3] = detail::hi8(weak);
        out[4] = detail::hi8(strong);
        return 63;
    case HidParser::SwitchProUsb:
        if (outCap < 10) { return 0; }
        detail::zero(out, 10);
        out[0] = 0x10; // rumble-only report
        out[1] = static_cast<std::uint8_t>(seq & 0x0FU);
        detail::switchEncodeMotor(&out[2], strong);
        detail::switchEncodeMotor(&out[6], weak);
        return 10;
    case HidParser::SteamController:
    case HidParser::GenericHid:
    case HidParser::None:
        return 0;
    }
    return 0;
}

// Lightbar colour. Mutates `st` for the DualSense's one-time setup handoff.
inline std::size_t buildLightbarReport(HidParser p, FeedbackState& st, std::uint8_t r,
                                       std::uint8_t g, std::uint8_t b, std::uint8_t* out,
                                       std::size_t outCap) {
    switch (p) {
    case HidParser::DualShock4:
        // Flag 0x02 alone marks the motor fields invalid, so a colour write
        // never stomps a rumble that is still running.
        if (outCap < 32) { return 0; }
        detail::zero(out, 32);
        out[0] = 0x05;
        out[1] = 0x02;
        out[6] = r;
        out[7] = g;
        out[8] = b;
        return 32;
    case HidParser::DualSense:
        if (outCap < 63) { return 0; }
        detail::zero(out, 63);
        out[0] = 0x02;
        out[2] = 0x04; // valid_flag1 LIGHTBAR_CONTROL_ENABLE
        if (!st.ds5LightbarSetupSent) {
            // Without this handoff the firmware keeps its own blue glow and the
            // host colour never appears. hid-playstation sends it at probe; the
            // Direct claim is the probe here, so it rides the first colour.
            out[39] = 0x02; // valid_flag2 LIGHTBAR_SETUP_CONTROL_ENABLE
            out[42] = 0x02; // lightbar_setup = LIGHT_OUT
            st.ds5LightbarSetupSent = true;
        }
        out[45] = r;
        out[46] = g;
        out[47] = b;
        return 63;
    case HidParser::SwitchProUsb:
    case HidParser::SteamController:
    case HidParser::GenericHid:
    case HidParser::None:
        return 0;
    }
    return 0;
}

// Player-indicator LEDs. `ledMask` bit 0 is the leftmost LED, as on the wire;
// each family is masked to the LEDs it physically has so a stray high bit
// cannot set a reserved firmware flag.
inline std::size_t buildPlayerLedsReport(HidParser p, std::uint8_t ledMask, std::uint8_t seq,
                                         std::uint8_t* out, std::size_t outCap) {
    switch (p) {
    case HidParser::DualSense:
        if (outCap < 63) { return 0; }
        detail::zero(out, 63);
        out[0] = 0x02;
        out[2] = 0x10; // valid_flag1 PLAYER_INDICATOR_CONTROL_ENABLE
        out[44] = static_cast<std::uint8_t>(ledMask & 0x1FU);
        return 63;
    case HidParser::SwitchProUsb:
        // Subcommand 0x30 (set player lights) rides the 0x01 rumble+subcommand
        // report; neutral rumble blocks keep the motors untouched.
        if (outCap < 12) { return 0; }
        detail::zero(out, 12);
        out[0] = 0x01;
        out[1] = static_cast<std::uint8_t>(seq & 0x0FU);
        detail::switchEncodeMotor(&out[2], 0);
        detail::switchEncodeMotor(&out[6], 0);
        out[10] = 0x30;
        out[11] = static_cast<std::uint8_t>(ledMask & 0x0FU);
        return 12;
    case HidParser::DualShock4:
    case HidParser::SteamController:
    case HidParser::GenericHid:
    case HidParser::None:
        return 0;
    }
    return 0;
}

// Adaptive-trigger effects. The two blocks are the game's own bytes, copied
// through untouched: the satellite forwards what the host wrote, and anything
// this end "corrected" would be a guess at firmware semantics.
//
// Note the wire order (left, right) is NOT the report order: the DualSense puts
// the right trigger's block first.
inline std::size_t buildTriggerEffectsReport(HidParser p,
                                             const std::uint8_t left[kTriggerEffectBlockBytes],
                                             const std::uint8_t right[kTriggerEffectBlockBytes],
                                             std::uint8_t* out, std::size_t outCap) {
    if (p != HidParser::DualSense) { return 0; }
    if (outCap < 63) { return 0; }
    detail::zero(out, 63);
    out[0] = 0x02;
    out[1] = 0x04 | 0x08; // valid_flag0: right + left trigger-effect blocks
    std::memcpy(out + 11, right, kTriggerEffectBlockBytes);
    std::memcpy(out + 22, left, kTriggerEffectBlockBytes);
    return 63;
}

} // namespace dish::input::usbout
