// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Byte-exact encoders for the Moonlight control-stream plaintexts this client
// sends, and decoders for the host->client events it handles. Layouts mirror
// Wolf's moonlight/control.hpp packed structs and the input-data protocol page;
// the unit tests pin them against the documented network fixtures.
//
// Encoders write at fixed offsets into a caller-owned buffer and never
// allocate, so the CONTROLLER_MULTI path can run on the input thread with a
// preallocated scratch buffer. Every encoder returns the bytes written.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

namespace dish::moonwire {

// Total plaintext sizes ([type u16][len u16] header included).
inline constexpr std::size_t kControllerMultiSize = 38;
inline constexpr std::size_t kControllerArrivalSize = 20;
inline constexpr std::size_t kControllerTouchSize = 32;
inline constexpr std::size_t kControllerMotionSize = 28;
inline constexpr std::size_t kControllerBatterySize = 16;
inline constexpr std::size_t kMouseMoveRelSize = 16;
inline constexpr std::size_t kPeriodicPingSize = 12;
inline constexpr std::size_t kTerminationSize = 8;

// Large enough for any plaintext this client encodes.
inline constexpr std::size_t kMaxPlaintextSize = 64;

// RTP hole-punch ping datagrams (plaintext UDP, not control-stream sealed).
inline constexpr std::size_t kRtpPingLegacySize = 4; // "PING"
inline constexpr std::size_t kRtpPingSize = 20;      // SS_PING{payload[16], seq}

// The hot-path report. `buttonFlags` is the effective 32-bit word; the encoder
// splits it into the legacy 16-bit field plus the buttonFlags2 extension.
// `activeMask` has a bit set per attached controller; dropping a bit tells the
// host that pad was unplugged. Sticks are Moonlight's frame: +Y is up.
std::size_t encodeControllerMulti(std::uint8_t* out, std::uint8_t controllerNumber,
                                  std::uint16_t activeMask, std::uint32_t buttonFlags,
                                  std::uint8_t leftTrigger, std::uint8_t rightTrigger,
                                  std::int16_t leftX, std::int16_t leftY, std::int16_t rightX,
                                  std::int16_t rightY);

// Announces a pad with its emulated type (moonproto::kControllerType*), its
// capability bitfield (moonproto::kCap*) and the buttons it can report.
std::size_t encodeControllerArrival(std::uint8_t* out, std::uint8_t controllerNumber,
                                    std::uint8_t controllerType, std::uint8_t capabilities,
                                    std::uint32_t supportedButtons);

// One pointer event on the emulated pad's touch surface. `x` / `y` are
// normalised 0..1 across the pad (the host multiplies by its emulated
// touchpad's resolution) and `pressure` is 1.0 for a solid contact, 0.0 on
// release. Unlike the satellite's full-state frame this is an EVENT stream, so
// the caller diffs first (MoonlightTouchDiffer.h).
std::size_t encodeControllerTouch(std::uint8_t* out, std::uint8_t controllerNumber,
                                  std::uint8_t eventType, std::uint32_t pointerId, float x, float y,
                                  float pressure);

// Motion sample. `motionType` is moonproto::kMotionAcceleration/Gyroscope; the
// components are IEEE-754 floats stored little-endian ("netfloat"). Units:
// m/s^2 for accel, deg/s for gyro.
std::size_t encodeControllerMotion(std::uint8_t* out, std::uint8_t controllerNumber,
                                   std::uint8_t motionType, float x, float y, float z);

// Battery report. `state` is a moonproto::kBattery* constant, `percentage`
// 0..100 or moonproto::kBatteryPercentageUnknown.
std::size_t encodeControllerBattery(std::uint8_t* out, std::uint8_t controllerNumber,
                                    std::uint8_t state, std::uint8_t percentage);

// Relative mouse motion; the deltas are BIG-endian on the wire, unlike
// everything else in the payload.
std::size_t encodeMouseMoveRel(std::uint8_t* out, std::int16_t deltaX, std::int16_t deltaY);

// Keep-alive, byte-for-byte the plaintext a real client sends.
std::size_t encodePeriodicPing(std::uint8_t* out);

// Graceful-quit notice with the standard reason code.
std::size_t encodeTermination(std::uint8_t* out);

// The RTP hole-punch ping datagram for the video/audio ports. When the host
// supplied an X-SS-Ping-Payload in RTSP SETUP (`payloadLen` > 0), the ping is
// the 20-byte SS_PING the host matches sessions by: the payload zero-padded or
// truncated to its fixed 16 bytes, then the sequence number little-endian.
// Without one it is the 4-byte legacy "PING". `out` needs kRtpPingSize bytes.
std::size_t encodeRtpPing(std::uint8_t* out, const char* payload, std::size_t payloadLen,
                          std::uint32_t sequence);

// ── Host -> client events ────────────────────────────────────────────────────

enum class HostEventType : std::uint8_t {
    Unknown, // a type this client does not handle: ignore gracefully
    Rumble,
    RumbleTriggers,
    MotionRequest,
    RgbLed,
    Termination,
};

struct HostEvent {
    HostEventType type = HostEventType::Unknown;

    std::uint16_t controllerNumber = 0;

    // Rumble: body motor magnitudes; RumbleTriggers reuses low/high as
    // left/right trigger magnitudes.
    std::uint16_t rumbleLow = 0;
    std::uint16_t rumbleHigh = 0;

    // MotionRequest: the host asks the client to START sending motion of
    // `motionType` at `motionRateHz` (0 stops it).
    std::uint16_t motionRateHz = 0;
    std::uint8_t motionType = 0;

    // RgbLed.
    std::uint8_t red = 0;
    std::uint8_t green = 0;
    std::uint8_t blue = 0;
};

// Decodes one decrypted control plaintext ([type u16 LE][len u16 LE][body]).
// nullopt means malformed (too short for its declared shape); a well-formed
// packet of an unhandled type comes back as HostEventType::Unknown so the
// caller can drop it without treating it as an error.
std::optional<HostEvent> decodeHostEvent(const std::uint8_t* data, std::size_t len);

} // namespace dish::moonwire
