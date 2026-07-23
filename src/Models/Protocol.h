// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Pure, Qt-free protocol-1 constants + tiny mappers shared by the wire layer,
// the REST DTO parsers and the pure reducers. Mirrors the authoritative
// satellite/src/core/types.h subset the CLIENT needs (opcodes, caps, apply
// results, close reasons, touchpad modes, controller types, crypto/timeout
// sizes). Ported from dish-windows src/core/model/Protocol.h — see
// satellite/docs/contract.md.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace dish::proto {

// REST + wire protocol version. Rides in every pairing/session request so any
// future change is gateable (contract §Versioning).
inline constexpr int kProtocolVersion = 1;

// ── UDP opcodes (contract §UDP messages) ────────────────────────────────────
// Topology-mutation opcodes 0x0004/5/6/7/8 and 0x000E are DELETED in
// protocol-1 — they are intentionally NOT defined here so a stray reference
// fails to compile. Up: INPUT/HEARTBEAT/MOTION/BATTERY/TOUCHPAD. Down:
// HEARTBEAT_ACK (enriched), RUMBLE, LIGHTBAR, SESSION_CLOSE.
inline constexpr std::uint16_t kMsgInput = 0x0001;        // c→s ctrlIdx + GamepadReport(12)
inline constexpr std::uint16_t kMsgHeartbeat = 0x0002;    // c→s empty
inline constexpr std::uint16_t kMsgHeartbeatAck = 0x0003; // s→c enriched (see below)
inline constexpr std::uint16_t kMsgRumble = 0x0009;       // s→c ctrlIdx + strong/weak/dur (BE)
inline constexpr std::uint16_t kMsgMotion = 0x000A;       // c→s ctrlIdx + 6×i16 + u32 (LE)
inline constexpr std::uint16_t kMsgBattery = 0x000B;      // c→s ctrlIdx + level + status
inline constexpr std::uint16_t kMsgTouchpad = 0x000C;     // c→s ctrlIdx + 15 bytes (see below)
inline constexpr std::uint16_t kMsgLightbar = 0x000D;     // s→c ctrlIdx + r + g + b
inline constexpr std::uint16_t kMsgSessionClose = 0x000F; // s→c reason(1)

// MSG_HEARTBEAT_ACK payload length (after the 4-byte inner type+len header):
// backendAvailable(1) + totalActiveControllers(1) + epoch(u16 BE) +
// activeBitmap(u16 BE) = 6 bytes. The epoch/bitmap pair drives the reconcile.
inline constexpr int kHeartbeatAckPayloadBytes = 6;

// MSG_TOUCHPAD payload length (after the 1-byte ctrlIdx): flags(1) +
// f0(id1+x2+y2) + f1(id1+x2+y2) + eventTimeMs(u32 LE) = 15 bytes. The trailing
// eventTimeMs is the protocol-1 addition (was 12); the server now requires
// msgLen >= 16 inner so a 12-byte body is dropped.
inline constexpr int kTouchpadPayloadBytes = 15;

// ── MSG_SESSION_CLOSE reason byte (contract §UDP messages) ───────────────────
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

// Inverse of touchpadModeName; unknown → off (the server's default too).
inline std::uint8_t touchpadModeFromName(std::string_view name) {
    if (name == "ds4") { return kTouchpadModeDs4; }
    if (name == "mouse") { return kTouchpadModeMouse; }
    return kTouchpadModeOff;
}

// ── Per-controller apply outcome (PUT/controller-PUT response) ───────────────
// Wire form is the lowercase string (protocol constant, never localized). The
// numeric code mirrors satellite APPLY_* so the old UDP-ack error mapping can
// be re-keyed onto strings.
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

// Parse a wire apply-result string into its code. An unrecognised string (a
// result a newer server invented) maps to kApplyUnknown rather than guessing
// success/failure — the caller treats it as not-live.
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

// A slot is LIVE (streams keep flowing) when the descriptor applied OK, or when
// a replug failed: the previous pad is left untouched and `appliedType` reports
// the type still in force. Every other code means the slot is not plugged.
inline bool applyResultSlotIsLive(std::uint8_t code) {
    return code == kApplyOk || code == kApplyReplugFailed;
}

// ── 401 machine-readable cause (contract §Error model) ──────────────────────
// Either code is TERMINAL: drop the key, surface "re-pair needed", stop
// retrying. Carried in the 401 body `{"error":"unauthorized","code":...}`.
inline constexpr std::string_view kAuthCodeNotPaired = "NOT_PAIRED";
inline constexpr std::string_view kAuthCodeBadProof = "BAD_PROOF";

// ── Host-feature deny reasons (descriptor grant, never localized) ───────────
inline constexpr std::string_view kHostDenyNotSupported = "notSupported";
inline constexpr std::string_view kHostDenyBackendUnavailable = "backendUnavailable";
inline constexpr std::string_view kHostDenyDenied = "denied";

} // namespace dish::proto
