// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Wire constants for the Moonlight (GameStream) host protocol spoken by
// Sunshine, Apollo and Wolf. Values follow the protocol documentation and host
// implementation of Wolf (MIT, games-on-whales/wolf): docs/modules/protocols
// and src/moonlight-protocol/moonlight/control.hpp. Where the two disagree the
// source is authoritative (TERMINATION is 0x0109 on the wire, not the 0x0100
// the overview page lists).

#pragma once

#include <cstdint>

namespace dish::moonproto {

// Fixed TCP ports. Everything else (RTSP, control, RTP) is dynamic: RTSP comes
// out of the launch response, the stream ports out of RTSP SETUP.
inline constexpr int kDefaultHttpPort = 47989;
inline constexpr int kDefaultHttpsPort = 47984;

// mDNS service Moonlight hosts advertise.
inline constexpr const char* kMdnsService = "_nvstream._tcp.local.";

// ── Control-stream packet types (outer, and decrypted inner) ─────────────────
// All little-endian on the wire.
inline constexpr std::uint16_t kPktEncrypted = 0x0001;
inline constexpr std::uint16_t kPktTermination = 0x0109;
inline constexpr std::uint16_t kPktPeriodicPing = 0x0200;
inline constexpr std::uint16_t kPktInputData = 0x0206;
inline constexpr std::uint16_t kPktRumbleData = 0x010B;
inline constexpr std::uint16_t kPktRumbleTriggers = 0x5500;
inline constexpr std::uint16_t kPktMotionEvent = 0x5501;
inline constexpr std::uint16_t kPktRgbLed = 0x5502;

// Graceful-quit reason, stored big-endian in the TERMINATION payload.
inline constexpr std::uint32_t kTerminateReasonGraceful = 0x80030023;

// ── INPUT_DATA input types (u32, little-endian on the wire) ──────────────────
inline constexpr std::uint32_t kInputMouseMoveRel = 0x00000007;
inline constexpr std::uint32_t kInputControllerMulti = 0x0000000C;
inline constexpr std::uint32_t kInputControllerArrival = 0x55000004;
inline constexpr std::uint32_t kInputControllerTouch = 0x55000005;
inline constexpr std::uint32_t kInputControllerMotion = 0x55000006;
inline constexpr std::uint32_t kInputControllerBattery = 0x55000007;

// ── CONTROLLER_ARRIVAL types (the "device to emulate" picker) ────────────────
inline constexpr std::uint8_t kControllerTypeUnknown = 0x00;
inline constexpr std::uint8_t kControllerTypeXbox = 0x01;
inline constexpr std::uint8_t kControllerTypePs = 0x02;
inline constexpr std::uint8_t kControllerTypeNintendo = 0x03;

// "Match the pad" — resolved against the bound input source before the wire, so
// it never travels. 0xFF and not 0x00: 0x00 is CONTROLLER_TYPE_UNKNOWN, a real
// wire value the host reads as "you decide", which is a different promise.
inline constexpr std::uint8_t kControllerTypeAuto = 0xFF;

// ── CONTROLLER_ARRIVAL capability bitfield ───────────────────────────────────
inline constexpr std::uint8_t kCapAnalogTriggers = 0x01;
inline constexpr std::uint8_t kCapRumble = 0x02;
inline constexpr std::uint8_t kCapTriggerRumble = 0x04;
inline constexpr std::uint8_t kCapTouchpad = 0x08;
inline constexpr std::uint8_t kCapAccelerometer = 0x10;
inline constexpr std::uint8_t kCapGyro = 0x20;
inline constexpr std::uint8_t kCapBattery = 0x40;
inline constexpr std::uint8_t kCapRgbLed = 0x80;

// ── CONTROLLER_MULTI button flags (effective = flags | (flags2 << 16)) ───────
inline constexpr std::uint32_t kBtnDpadUp = 0x0001;
inline constexpr std::uint32_t kBtnDpadDown = 0x0002;
inline constexpr std::uint32_t kBtnDpadLeft = 0x0004;
inline constexpr std::uint32_t kBtnDpadRight = 0x0008;
inline constexpr std::uint32_t kBtnStart = 0x0010;
inline constexpr std::uint32_t kBtnBack = 0x0020;
inline constexpr std::uint32_t kBtnLeftStick = 0x0040;
inline constexpr std::uint32_t kBtnRightStick = 0x0080;
inline constexpr std::uint32_t kBtnLeftButton = 0x0100;
inline constexpr std::uint32_t kBtnRightButton = 0x0200;
inline constexpr std::uint32_t kBtnHome = 0x0400;
inline constexpr std::uint32_t kBtnA = 0x1000;
inline constexpr std::uint32_t kBtnB = 0x2000;
inline constexpr std::uint32_t kBtnX = 0x4000;
inline constexpr std::uint32_t kBtnY = 0x8000;
inline constexpr std::uint32_t kBtnPaddle1 = 0x010000;
inline constexpr std::uint32_t kBtnPaddle2 = 0x020000;
inline constexpr std::uint32_t kBtnPaddle3 = 0x040000;
inline constexpr std::uint32_t kBtnPaddle4 = 0x080000;
inline constexpr std::uint32_t kBtnTouchpad = 0x100000;
inline constexpr std::uint32_t kBtnMisc = 0x200000;

// Named in neither Wolf's table nor its control.hpp, and set in what every
// shipping client advertises. Carried so the advertised word is the whole low
// half rather than a hole a host might read as a missing button.
inline constexpr std::uint32_t kBtnReservedLow = 0x0800;

// The support_button_flags a standard pad advertises in CONTROLLER_ARRIVAL: the
// whole 16-bit legacy word (dpad, Start/Back, sticks, shoulders, Home, ABXY and
// the one reserved bit). A live Sunshine host logs it back as
// supportedButtonFlags [0000FFFF]; the three Dish clients advertise this one
// value so a host cannot see three different pads.
inline constexpr std::uint32_t kStandardButtons =
    kBtnDpadUp | kBtnDpadDown | kBtnDpadLeft | kBtnDpadRight | kBtnStart | kBtnBack |
    kBtnLeftStick | kBtnRightStick | kBtnLeftButton | kBtnRightButton | kBtnHome | kBtnReservedLow |
    kBtnA | kBtnB | kBtnX | kBtnY;

// ── CONTROLLER_TOUCH event types (control.hpp TOUCH_EVENT_TYPE) ──────────────
// Shared by the touchscreen, pen and controller-touchpad surfaces. Only the
// three a two-finger pad can produce are modelled; HOVER and the pen-only
// values have no source here.
inline constexpr std::uint8_t kTouchEventDown = 0x01;
inline constexpr std::uint8_t kTouchEventUp = 0x02;
inline constexpr std::uint8_t kTouchEventMove = 0x03;

// ── CONTROLLER_MOTION types (also carried by the host's MOTION_EVENT) ────────
inline constexpr std::uint8_t kMotionAcceleration = 0x01;
inline constexpr std::uint8_t kMotionGyroscope = 0x02;

// ── CONTROLLER_BATTERY states ────────────────────────────────────────────────
inline constexpr std::uint8_t kBatteryStateUnknown = 0x00;
inline constexpr std::uint8_t kBatteryNotPresent = 0x01;
inline constexpr std::uint8_t kBatteryDischarging = 0x02;
inline constexpr std::uint8_t kBatteryCharging = 0x03;
inline constexpr std::uint8_t kBatteryNotCharging = 0x04;
inline constexpr std::uint8_t kBatteryFull = 0x05;
inline constexpr std::uint8_t kBatteryPercentageUnknown = 0xFF;

} // namespace dish::moonproto
