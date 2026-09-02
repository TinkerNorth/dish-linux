// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Protocol-1 constants and mappers shared by the wire layer, the REST DTO parsers
// and the reducers. The client-side subset of the authoritative
// satellite/src/core/types.h; see satellite/docs/contract.md.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace dish::proto {

// Rides in every pairing/session request. The satellite accepts the whole
// [supportedMin, supported] range, settles each session on the client's offer
// and echoes the settled version back; an offer outside its range is refused
// with 409 + `supported`/`supportedMin` (see reducer/ProtocolNegotiation.h).
inline constexpr int kProtocolVersion = 2;

// The oldest version this client still speaks end to end. A session that
// settles on 1 streams the 16-byte TOUCHPAD frame and gets no feedback return
// paths; the client never offers below this.
inline constexpr int kProtocolVersionMin = 1;

// Whether a settled session speaks the v2 frames. Every version gate in the
// client goes through this, so raising the floor is one edit.
inline constexpr bool settledSpeaksV2(int settledVersion) { return settledVersion >= 2; }

// ── UDP opcodes ─────────────────────────────────────────────────────────────
// The topology-mutation opcodes 0x0004..0x0008 and 0x000E are gone in
// protocol-1; leaving them undefined makes a stray reference fail to compile.
inline constexpr std::uint16_t kMsgInput = 0x0001;        // c→s ctrlIdx + GamepadReport(12)
inline constexpr std::uint16_t kMsgHeartbeat = 0x0002;    // c→s empty
inline constexpr std::uint16_t kMsgHeartbeatAck = 0x0003; // s→c enriched (see below)
inline constexpr std::uint16_t kMsgRumble = 0x0009;       // s→c ctrlIdx + strong/weak/dur (BE)
inline constexpr std::uint16_t kMsgMotion = 0x000A;       // c→s ctrlIdx + 6×i16 + u32 (LE)
inline constexpr std::uint16_t kMsgBattery = 0x000B;      // c→s ctrlIdx + level + status
inline constexpr std::uint16_t kMsgTouchpad = 0x000C;     // c→s ctrlIdx + 15 (v1) or 18 (v2)
inline constexpr std::uint16_t kMsgLightbar = 0x000D;     // s→c ctrlIdx + r + g + b
inline constexpr std::uint16_t kMsgSessionClose = 0x000F; // s→c reason(1)
// Protocol-2 feedback return paths. Both are caps-gated: the satellite sends
// them only to a session whose descriptor advertised the matching actuator, so
// a client that never advertises never has to handle them.
inline constexpr std::uint16_t kMsgTriggerEffects = 0x0010; // s→c ctrlIdx + left(11) + right(11)
inline constexpr std::uint16_t kMsgPlayerLeds = 0x0011;     // s→c ctrlIdx + ledMask(1)
// Controller audio: the EMULATED pad's own endpoints, never the host's game
// audio. Both audio messages carry ctrlIdx(1) + seq(u16 BE) + exactly one 20 ms
// Opus packet; `seq` wraps and buys only gap detection and late-drop inside the
// receiver's 2-frame reorder window (core/audio/AudioJitter.h) — no acks, no
// retransmits, Opus in-band FEC + PLC conceal loss.
inline constexpr std::uint16_t kMsgMicAudio = 0x0012;     // c→s the pad's headset mic, mono
inline constexpr std::uint16_t kMsgSpeakerAudio = 0x0013; // s→c the pad's speaker out, stereo
// Mic-mute lamp (s→c): ctrlIdx(1) + state(1), kMicLedState*. Coalesced
// last-value-wins server-side like the lightbar. Gated on kCapMic rather than a
// cap of its own: a mute lamp without a microphone behind it is dead metal.
inline constexpr std::uint16_t kMsgMicLed = 0x0014;

// After the 4-byte inner type+len header: backendAvailable(1) +
// totalActiveControllers(1) + epoch(u16 BE) + activeBitmap(u16 BE).
inline constexpr int kHeartbeatAckPayloadBytes = 6;

// After the 1-byte ctrlIdx: flags(1) + f0(id1+x2+y2) + f1(id1+x2+y2) +
// eventTimeMs(u32 LE). The server requires an inner msgLen >= 16, so the
// pre-protocol-1 12-byte body without eventTimeMs is dropped.
inline constexpr int kTouchpadPayloadBytes = 15;

// Protocol 2 reshapes 0x000C into the POINTER frame, 19 bytes total: the click
// moved out of the finger flags into its own buttons byte and a signed vertical
// wheel was appended. The receiver disambiguates by frame length, so the two
// generations coexist on one opcode.
// After ctrlIdx: fingerFlags(1) + buttons(1) + f0(id1+x2+y2) + f1(id1+x2+y2) +
// eventTimeMs(u32 LE) + scrollV(i16 LE).
inline constexpr int kPointerPayloadBytes = 18;

// POINTER buttons byte.
inline constexpr std::uint8_t kPointerButtonLeft = 0x01;
inline constexpr std::uint8_t kPointerButtonRight = 0x02;
inline constexpr std::uint8_t kPointerButtonMiddle = 0x04;

// POINTER fingerFlags byte. The click bit (0x04 in the v1 flags byte) is gone:
// it lives in the buttons byte now.
inline constexpr std::uint8_t kPointerFinger0Active = 0x01;
inline constexpr std::uint8_t kPointerFinger1Active = 0x02;

// scrollV is an event, not a level: one wheel notch is 120 and a resend carries
// 0. Nothing in this client synthesizes a wheel today (see PointerRouting.h).
inline constexpr std::int16_t kScrollUnitsPerNotch = 120;

// MSG_TRIGGER_EFFECTS payload after the 1-byte ctrlIdx: two 11-byte DualSense
// trigger-effect blocks, left then right. The bytes are the game's own output
// report fields, forwarded verbatim by the satellite and replayed verbatim into
// the pad, so nothing here interprets them.
inline constexpr int kTriggerEffectBlockBytes = 11;
inline constexpr int kTriggerEffectsPayloadBytes = 2 * kTriggerEffectBlockBytes;

// MSG_PLAYER_LEDS payload after the 1-byte ctrlIdx: the indicator bitmask,
// bit 0 = leftmost LED (DualSense uses bits 0-4, Switch Pro bits 0-3).
inline constexpr int kPlayerLedsPayloadBytes = 1;

// ── Datagram ceilings ───────────────────────────────────────────────────────
// One Ethernet MTU in either direction, so a full audio frame crosses a LAN
// without fragmenting (a fragmented Opus packet would be an all-or-nothing loss
// anyway). What the crypto framing leaves behind is the ceiling every sender
// must stay under; the satellite pins the same number and truncates a longer
// read, which then fails the AEAD.
inline constexpr std::size_t kUdpDatagramMaxBytes = 1500;
inline constexpr std::size_t kUdpOuterHeaderBytes = 8; // token(4) + counter(4 BE)
inline constexpr std::size_t kUdpInnerHeaderBytes = 4; // msgType(2 BE) + msgLen(2 BE)
inline constexpr std::size_t kUdpAeadTagBytes = 16;    // ChaCha20-Poly1305-IETF
inline constexpr std::size_t kUdpMaxInnerPayloadBytes =
    kUdpDatagramMaxBytes - kUdpOuterHeaderBytes - kUdpInnerHeaderBytes - kUdpAeadTagBytes;

// ── Controller audio wire format (kMsgMicAudio / kMsgSpeakerAudio) ──────────
// Fixed on the wire, never negotiated: Opus resamples to 48 kHz internally
// regardless, so pinning the rate costs nothing and spares both ends a
// resampler; 20 ms is also the emulated pad's USB-audio service interval, so no
// side re-windows. Mirrors satellite core/types.h AUDIO_*.
inline constexpr int kAudioSampleRateHz = 48000;
inline constexpr int kAudioFrameMs = 20;
inline constexpr int kAudioFrameSamples = kAudioSampleRateHz / 1000 * kAudioFrameMs; // per channel
// Mic is the pad's headset microphone (mono); speaker is channels 1/2 of the
// DualSense 4-channel OUT stream, its speaker and headset jack (stereo).
inline constexpr int kAudioMicChannels = 1;
inline constexpr int kAudioSpeakerChannels = 2;

// Both audio messages: ctrlIdx(1) + seq(u16 BE) ahead of the Opus bytes.
inline constexpr int kAudioWireHeaderBytes = 3;
// Smallest frame worth dispatching: the header plus at least one Opus byte. A
// 1-byte Opus packet is legal (a valid DTX silence frame, and the mic encoder
// really does emit those); a header with nothing behind it is malformed.
inline constexpr int kAudioWireMinPayloadBytes = kAudioWireHeaderBytes + 1;
// Largest Opus packet one datagram can carry. Two orders of magnitude above a
// real 20 ms packet (~80 bytes mic, ~240 speaker); a bound, not a target.
inline constexpr std::size_t kAudioWireMaxOpusBytes =
    kUdpMaxInnerPayloadBytes - static_cast<std::size_t>(kAudioWireHeaderBytes);

// kMsgMicLed state byte. Pulse is the DualSense's own breathing pattern, which
// the pad renders itself; the satellite only forwards which mode the game asked
// for. Anything else is malformed and dropped rather than guessed at.
inline constexpr std::uint8_t kMicLedStateOff = 0;
inline constexpr std::uint8_t kMicLedStateOn = 1;
inline constexpr std::uint8_t kMicLedStatePulse = 2;
inline constexpr std::uint8_t kMicLedStateCount = 3;
// After the 1-byte ctrlIdx: state(1).
inline constexpr int kMicLedPayloadBytes = 1;

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
// Protocol-2 additions. They advertise the CLIENT's actuator: the satellite
// sends 0x0010 / 0x0011 only to a session that claimed it can land them.
inline constexpr std::uint16_t kCapTriggerEffects = 0x0010;
inline constexpr std::uint16_t kCapPlayerLeds = 0x0020;
// Controller audio. Same house rule: a cap advertises the CLIENT's own
// actuator/source — kCapMic says this client sources kMsgMicAudio (and takes
// the kMsgMicLed return path, which rides it), kCapSpeaker that it accepts
// kMsgSpeakerAudio. Independent directions; neither implies the other.
inline constexpr std::uint16_t kCapMic = 0x0040;
inline constexpr std::uint16_t kCapSpeaker = 0x0080;

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
