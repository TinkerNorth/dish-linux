// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Protocol-1 constants and mappers shared by the wire layer, the REST DTO parsers
// and the reducers. The client-side subset of the authoritative
// satellite/src/core/types.h; see satellite/docs/contract.md.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace dish::proto {

// Rides in every pairing/session request so a future change is gateable.
inline constexpr int kProtocolVersion = 1;

// ── UDP opcodes ─────────────────────────────────────────────────────────────
// The topology-mutation opcodes 0x0004..0x0008 and 0x000E are gone in
// protocol-1; leaving them undefined makes a stray reference fail to compile.
inline constexpr std::uint16_t kMsgInput = 0x0001;        // c→s ctrlIdx + GamepadReport(12)
inline constexpr std::uint16_t kMsgHeartbeat = 0x0002;    // c→s empty
inline constexpr std::uint16_t kMsgHeartbeatAck = 0x0003; // s→c enriched (see below)
inline constexpr std::uint16_t kMsgRumble = 0x0009;       // s→c ctrlIdx + strong/weak/dur (BE)
inline constexpr std::uint16_t kMsgMotion = 0x000A;       // c→s ctrlIdx + 6×i16 + u32 (LE)
inline constexpr std::uint16_t kMsgBattery = 0x000B;      // c→s ctrlIdx + level + status
inline constexpr std::uint16_t kMsgTouchpad = 0x000C;     // c→s ctrlIdx + 15 bytes (see below)
inline constexpr std::uint16_t kMsgLightbar = 0x000D;     // s→c ctrlIdx + r + g + b
inline constexpr std::uint16_t kMsgSessionClose = 0x000F; // s→c reason(1)

// After the 4-byte inner type+len header: backendAvailable(1) +
// totalActiveControllers(1) + epoch(u16 BE) + activeBitmap(u16 BE).
inline constexpr int kHeartbeatAckPayloadBytes = 6;

// After the 1-byte ctrlIdx: flags(1) + f0(id1+x2+y2) + f1(id1+x2+y2) +
// eventTimeMs(u32 LE). The server requires an inner msgLen >= 16, so the
// pre-protocol-1 12-byte body without eventTimeMs is dropped.
inline constexpr int kTouchpadPayloadBytes = 15;

// ── MSG_SESSION_CLOSE reason byte ───────────────────────────────────────────
inline constexpr std::uint8_t kCloseReasonShutdown = 0; // server going down
inline constexpr std::uint8_t kCloseReasonKicked = 1;   // admin kick (transient)
inline constexpr std::uint8_t kCloseReasonReplaced = 2; // superseded by a newer PUT
inline constexpr std::uint8_t kCloseReasonUnpaired = 3; // trust revoked (terminal)

// ── Controller capability bits (descriptor caps word) ───────────────────────
inline constexpr std::uint16_t kCapAnalogTriggers = 0x0001;
inline constexpr std::uint16_t kCapRumble = 0x0002;
inline constexpr std::uint16_t kCapMotion = 0x0004;
inline constexpr std::uint16_t kCapLightbar = 0x0008;

// ── Controller types (catalog ids / descriptor `type`) ──────────────────────
inline constexpr std::uint8_t kControllerTypeXbox = 0;
inline constexpr std::uint8_t kControllerTypePlayStation = 1;
inline constexpr std::uint8_t kControllerTypeDualSense = 2;
inline constexpr std::uint8_t kControllerTypeSwitchPro = 3;

// ── Touchpad routing modes (descriptor `touchpadMode`, wire strings) ─────────
inline constexpr std::uint8_t kTouchpadModeDs4 = 0;
inline constexpr std::uint8_t kTouchpadModeMouse = 1;
inline constexpr std::uint8_t kTouchpadModeOff = 2;

inline std::string_view touchpadModeName(std::uint8_t mode) {
    switch (mode) {
    case kTouchpadModeMouse:
        return "mouse";
    case kTouchpadModeOff:
        return "off";
    case kTouchpadModeDs4:
    default:
        return "ds4";
    }
}

// Unknown maps to off, matching the server's default.
inline std::uint8_t touchpadModeFromName(std::string_view name) {
    if (name == "ds4") { return kTouchpadModeDs4; }
    if (name == "mouse") { return kTouchpadModeMouse; }
    return kTouchpadModeOff;
}

// ── Per-controller apply outcome (PUT/controller-PUT response) ───────────────
// Wire form is the lowercase string, never localized; the numeric codes mirror
// satellite APPLY_*.
inline constexpr std::uint8_t kApplyOk = 0;
inline constexpr std::uint8_t kApplyNoSlots = 1;
inline constexpr std::uint8_t kApplyPluginFailed = 2;
inline constexpr std::uint8_t kApplyReplugFailed = 3;
inline constexpr std::uint8_t kApplyBackendUnavailable = 4;
inline constexpr std::uint8_t kApplyInvalidType = 5;
inline constexpr std::uint8_t kApplyInvalidIndex = 6;
inline constexpr std::uint8_t kApplyUnknown = 0xFF; // unrecognised string from a newer server

inline std::string_view applyResultName(std::uint8_t code) {
    switch (code) {
    case kApplyOk:
        return "ok";
    case kApplyNoSlots:
        return "noSlots";
    case kApplyPluginFailed:
        return "pluginFailed";
    case kApplyReplugFailed:
        return "replugFailed";
    case kApplyBackendUnavailable:
        return "backendUnavailable";
    case kApplyInvalidType:
        return "invalidType";
    case kApplyInvalidIndex:
        return "invalidIndex";
    default:
        return "unknown";
    }
}

// A result a newer server invented maps to kApplyUnknown rather than guessing
// success or failure; the caller treats it as not-live.
inline std::uint8_t applyResultFromName(std::string_view name) {
    if (name == "ok") { return kApplyOk; }
    if (name == "noSlots") { return kApplyNoSlots; }
    if (name == "pluginFailed") { return kApplyPluginFailed; }
    if (name == "replugFailed") { return kApplyReplugFailed; }
    if (name == "backendUnavailable") { return kApplyBackendUnavailable; }
    if (name == "invalidType") { return kApplyInvalidType; }
    if (name == "invalidIndex") { return kApplyInvalidIndex; }
    return kApplyUnknown;
}

// A failed replug still counts as live: the previous pad is left untouched and
// `appliedType` reports the type still in force.
inline bool applyResultSlotIsLive(std::uint8_t code) {
    return code == kApplyOk || code == kApplyReplugFailed;
}

// ── 401 machine-readable cause, from the body's `code` field ─────────────────
// Either code is terminal: drop the key, surface "re-pair needed", stop retrying.
inline constexpr std::string_view kAuthCodeNotPaired = "NOT_PAIRED";
inline constexpr std::string_view kAuthCodeBadProof = "BAD_PROOF";

// ── Host-feature deny reasons (wire strings, never localized) ───────────────
inline constexpr std::string_view kHostDenyNotSupported = "notSupported";
inline constexpr std::string_view kHostDenyBackendUnavailable = "backendUnavailable";
inline constexpr std::string_view kHostDenyDenied = "denied";

} // namespace dish::proto
