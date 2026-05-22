// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

// Coverage for the motion-honesty + reactive-caps wire surface:
//
//   * `SatelliteClient::withMotionCapability` — the cap-bit derivation that
//     keeps CAP_MOTION honest (only set when the bound pad has an IMU);
//   * `SatelliteClient::encodeCapsUpdatePayload` — the pure encoder for
//     MSG_CONTROLLER_CAPS_UPDATE (0x000E) which mirrors the caps field of
//     MSG_CONTROLLER_ADD but updates an already-registered controller in
//     place; and
//   * `SatelliteClient::parseControllerAckPayload` — the pure decoder for
//     MSG_CONTROLLER_ACK (0x0006) covering both the legacy 4-byte payload
//     (pre-extension satellite) and the extended 5-byte payload that carries
//     the motion-flags byte (PR satellite#34).
//
// Same style as test_satellite_client_lightbar / _rumble: drive only the
// pure static helpers so we never need a live socket or decryption seam.

#include "Network/SatelliteClient.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <optional>

using dish::net::SatelliteClient;

// ---------------------------------------------------------------------------
// CAP_MOTION honesty — withMotionCapability mirrors withLightbarCapability:
// the only on-the-wire bit-flip is bit 0x0004, and only when the pad has a
// real gyro/accel. The dish-side honesty rule (SDLGamepadBridge sets
// motionCapable_ iff SDL exposes the sensor) is unit-testable via this
// helper because the helper is the one place the bit is computed.
// ---------------------------------------------------------------------------

TEST_CASE("CAP_MOTION constant pins the wire value", "[motion][caps]") {
    REQUIRE(SatelliteClient::kCapMotion == 0x0004);
    REQUIRE((SatelliteClient::kCapMotion & SatelliteClient::kCapAnalogTriggers) == 0);
    REQUIRE((SatelliteClient::kCapMotion & SatelliteClient::kCapRumble) == 0);
    REQUIRE((SatelliteClient::kCapMotion & SatelliteClient::kCapLightbar) == 0);
}

TEST_CASE("withMotionCapability sets bit 0x0004 iff the pad has gyro/accel", "[motion][caps]") {
    // Base word the dish advertises today: analog triggers | rumble == 0x0003.
    // Neither CAP_MOTION nor CAP_LIGHTBAR is in here — both are per-controller.
    const std::uint16_t base = SatelliteClient::kCapAnalogTriggers | SatelliteClient::kCapRumble;
    REQUIRE(base == 0x0003);

    SECTION("controller has an IMU -> CAP_MOTION is OR-ed in") {
        const std::uint16_t caps = SatelliteClient::withMotionCapability(base, true);
        REQUIRE(caps == 0x0007);
        REQUIRE((caps & SatelliteClient::kCapMotion) != 0);
    }
    SECTION("controller has no IMU -> word is unchanged (honesty bit)") {
        // The load-bearing case: an Xbox 360 / Xbox One pad must NOT advertise
        // CAP_MOTION even though the user asked for motion forwarding. SDL's
        // SDL_GameControllerHasSensor returns SDL_FALSE for those pads, so
        // SDLGamepadBridge leaves motionCapable_ == false and the bit stays
        // clear here. Mirrors the dish-android composer's toCapBits.
        const std::uint16_t caps = SatelliteClient::withMotionCapability(base, false);
        REQUIRE(caps == 0x0003);
        REQUIRE((caps & SatelliteClient::kCapMotion) == 0);
    }
}

TEST_CASE("withMotionCapability leaves the other capability bits intact", "[motion][caps]") {
    // The helper must only ever toggle bit 0x0004, never clear or shift another.
    for (std::uint16_t base : {std::uint16_t{0x0000}, std::uint16_t{0x0003}, std::uint16_t{0x0008},
                               std::uint16_t{0xFFFB}}) {
        const std::uint16_t with = SatelliteClient::withMotionCapability(base, true);
        const std::uint16_t without = SatelliteClient::withMotionCapability(base, false);
        // "without" is a pure identity — the dish stays honest by default.
        REQUIRE(without == base);
        // "with" differs from the base only in bit 0x0004.
        REQUIRE((with & ~static_cast<std::uint16_t>(0x0004)) == base);
        REQUIRE((with | static_cast<std::uint16_t>(0x0004)) == with);
    }
}

TEST_CASE("withMotionCapability is idempotent when the bit is already set", "[motion][caps]") {
    const std::uint16_t base = 0x0003 | SatelliteClient::kCapMotion;
    REQUIRE(SatelliteClient::withMotionCapability(base, true) == base);
    REQUIRE(SatelliteClient::withMotionCapability(base, false) == base);
}

// ---------------------------------------------------------------------------
// MSG_CONTROLLER_CAPS_UPDATE (0x000E) — pure encoder. Wire shape mirrors the
// caps field of MSG_CONTROLLER_ADD: ctrlIdx(1) + caps(2 BE) = 3 bytes.
// Same layout as the producer in dish-android satellite_jni.cpp and the
// consumer in satellite/src/net/inner_dispatch.cpp.
// ---------------------------------------------------------------------------

TEST_CASE("MSG_CONTROLLER_CAPS_UPDATE constant pins the wire value", "[caps-update]") {
    REQUIRE(SatelliteClient::kMsgControllerCapsUpdate == 0x000E);
}

TEST_CASE("encodeCapsUpdatePayload writes ctrlIdx + caps BE16", "[caps-update]") {
    SECTION("zero index, no caps") {
        const auto p = SatelliteClient::encodeCapsUpdatePayload(/*ctrlIdx=*/0, /*caps=*/0x0000);
        REQUIRE(p.size() == 3U);
        REQUIRE(p[0] == 0x00U);
        REQUIRE(p[1] == 0x00U);
        REQUIRE(p[2] == 0x00U);
    }
    SECTION("non-zero index + the full base+motion+lightbar word") {
        // 0x000F == triggers | rumble | motion | lightbar, encoded BE.
        const auto p = SatelliteClient::encodeCapsUpdatePayload(/*ctrlIdx=*/3, /*caps=*/0x000F);
        REQUIRE(p[0] == 0x03U);
        REQUIRE(p[1] == 0x00U); // MSB
        REQUIRE(p[2] == 0x0FU); // LSB
    }
    SECTION("full 16-bit caps word survives without byte-swap") {
        const auto p = SatelliteClient::encodeCapsUpdatePayload(/*ctrlIdx=*/0xFF, /*caps=*/0xABCD);
        REQUIRE(p[0] == 0xFFU);
        REQUIRE(p[1] == 0xABU);
        REQUIRE(p[2] == 0xCDU);
    }
}

TEST_CASE("encodeCapsUpdatePayload matches the caps field of MSG_CONTROLLER_ADD", "[caps-update]") {
    // The receiver shares its decode path with MSG_CONTROLLER_ADD — the dish
    // promises the same wire shape, just a different msgType. Compare against
    // a hand-built MSG_CONTROLLER_ADD-style payload to keep that contract
    // pinned: ctrlIdx(1) + caps(2 BE) verbatim.
    const std::uint8_t ctrlIdx = 7;
    const std::uint16_t caps = SatelliteClient::kCapAnalogTriggers | SatelliteClient::kCapRumble |
                               SatelliteClient::kCapMotion;
    const auto p = SatelliteClient::encodeCapsUpdatePayload(ctrlIdx, caps);
    REQUIRE(p[0] == ctrlIdx);
    REQUIRE(p[1] == 0x00U);
    REQUIRE(p[2] == static_cast<std::uint8_t>(caps & 0xFFU));
}

// ---------------------------------------------------------------------------
// MSG_CONTROLLER_ACK (0x0006) — pure decoder. Covers both wire shapes:
//   * legacy 4-byte payload: reqType(2 BE) + ctrlIdx + result
//   * extended 5-byte payload: + motionFlags (PR satellite#34)
// A pre-extension satellite ships only the four bytes; the dish-side optional
// stays std::nullopt rather than being misread as motionFlags == 0, which
// would mean "backend broken" against an old satellite.
// ---------------------------------------------------------------------------

namespace {

// Build a 4-byte legacy ACK payload (pre-extension satellite).
std::array<std::uint8_t, 4> legacyAckPayload(std::uint16_t reqType, std::uint8_t idx,
                                             std::uint8_t result) {
    return {
        static_cast<std::uint8_t>(reqType >> 8),
        static_cast<std::uint8_t>(reqType & 0xFF),
        idx,
        result,
    };
}

// Build a 5-byte extended ACK payload (post-extension satellite).
std::array<std::uint8_t, 5> extendedAckPayload(std::uint16_t reqType, std::uint8_t idx,
                                               std::uint8_t result, std::uint8_t motionFlags) {
    return {
        static_cast<std::uint8_t>(reqType >> 8),
        static_cast<std::uint8_t>(reqType & 0xFF),
        idx,
        result,
        motionFlags,
    };
}

} // namespace

TEST_CASE("parseControllerAckPayload decodes the legacy 4-byte form", "[controller-ack]") {
    const auto p = legacyAckPayload(/*reqType=*/SatelliteClient::kMsgControllerAdd,
                                    /*idx=*/2, /*result=*/0x00);
    const auto ack = SatelliteClient::parseControllerAckPayload(p.data(), p.size());
    REQUIRE(ack.has_value());
    REQUIRE(ack->requestType == SatelliteClient::kMsgControllerAdd);
    REQUIRE(ack->controllerIndex == 2);
    REQUIRE(ack->result == 0x00U);
    // Critical: motionFlags must be std::nullopt — NOT 0 — when the wire
    // payload is the legacy 4-byte form. A pre-extension satellite never
    // sends the byte, and treating its absence as 0 would mean "backend
    // broken" for every old satellite. The dish-side store collapses
    // nullopt onto "unknown" instead.
    REQUIRE_FALSE(ack->motionFlags.has_value());
}

TEST_CASE("parseControllerAckPayload decodes the extended 5-byte form", "[controller-ack]") {
    const auto p =
        extendedAckPayload(/*reqType=*/SatelliteClient::kMsgControllerAdd,
                           /*idx=*/0, /*result=*/0x00,
                           /*motionFlags=*/SatelliteClient::kAckMotionFlagSinkSupportedForType |
                               SatelliteClient::kAckMotionFlagBackendOk);
    const auto ack = SatelliteClient::parseControllerAckPayload(p.data(), p.size());
    REQUIRE(ack.has_value());
    REQUIRE(ack->requestType == SatelliteClient::kMsgControllerAdd);
    REQUIRE(ack->controllerIndex == 0);
    REQUIRE(ack->result == 0x00U);
    REQUIRE(ack->motionFlags.has_value());
    REQUIRE((*ack->motionFlags & SatelliteClient::kAckMotionFlagSinkSupportedForType) != 0);
    REQUIRE((*ack->motionFlags & SatelliteClient::kAckMotionFlagBackendOk) != 0);
}

TEST_CASE("parseControllerAckPayload distinguishes the two motion-flag bits", "[controller-ack]") {
    SECTION("only bit 0 (sink-supported-for-type) set — backend missing the sink") {
        const auto p = extendedAckPayload(SatelliteClient::kMsgControllerAdd, 0, 0x00,
                                          SatelliteClient::kAckMotionFlagSinkSupportedForType);
        const auto ack = SatelliteClient::parseControllerAckPayload(p.data(), p.size());
        REQUIRE(ack.has_value());
        REQUIRE(ack->motionFlags.has_value());
        REQUIRE((*ack->motionFlags & SatelliteClient::kAckMotionFlagSinkSupportedForType) != 0);
        REQUIRE((*ack->motionFlags & SatelliteClient::kAckMotionFlagBackendOk) == 0);
    }
    SECTION("neither bit set — backend has no IMU for this controller type") {
        const auto p = extendedAckPayload(SatelliteClient::kMsgControllerAdd, 0, 0x00, 0x00);
        const auto ack = SatelliteClient::parseControllerAckPayload(p.data(), p.size());
        REQUIRE(ack.has_value());
        REQUIRE(ack->motionFlags.has_value());
        // A zero flags byte is still definitive data ("backend has no IMU"),
        // distinct from std::nullopt which means "we don't know yet". The
        // dish surfaces the former as a user-visible notice; the latter is
        // silent.
        REQUIRE(*ack->motionFlags == 0x00U);
    }
}

TEST_CASE("parseControllerAckPayload rejects a truncated payload", "[controller-ack]") {
    const std::uint8_t p[3] = {0x00, 0x04, 0x00};
    const auto ack = SatelliteClient::parseControllerAckPayload(p, sizeof(p));
    REQUIRE_FALSE(ack.has_value());
}

TEST_CASE("parseControllerAckPayload rejects a null pointer", "[controller-ack]") {
    const auto ack = SatelliteClient::parseControllerAckPayload(nullptr, 8);
    REQUIRE_FALSE(ack.has_value());
}

TEST_CASE("parseControllerAckPayload tolerates extra trailing bytes", "[controller-ack]") {
    // Forward-compat: a future satellite extension that tacks more bytes on
    // (e.g. lightbar status) must not break the dish-side decode. The
    // motion-flags byte at offset 4 is captured; anything past that is
    // ignored verbatim.
    const std::uint8_t p[8] = {
        static_cast<std::uint8_t>(SatelliteClient::kMsgControllerAdd >> 8),
        static_cast<std::uint8_t>(SatelliteClient::kMsgControllerAdd & 0xFF),
        0x01,                                     // ctrlIdx
        0x00,                                     // result == ACK_OK
        SatelliteClient::kAckMotionFlagBackendOk, // motion flags
        0xDE,
        0xAD,
        0xBE // future-extension bytes
    };
    const auto ack = SatelliteClient::parseControllerAckPayload(p, sizeof(p));
    REQUIRE(ack.has_value());
    REQUIRE(ack->controllerIndex == 1);
    REQUIRE(ack->result == 0x00U);
    REQUIRE(ack->motionFlags.has_value());
    REQUIRE(*ack->motionFlags == SatelliteClient::kAckMotionFlagBackendOk);
}

TEST_CASE("ACK_MOTION_FLAG_* constants pin the wire values", "[controller-ack]") {
    // The dish + satellite must agree on these bit positions byte-for-byte —
    // they're the only contract between the two sides on the motion-status
    // axis. A drift would silently re-interpret a "backend missing" satellite
    // as "kernel rejected the sink" and vice versa.
    REQUIRE(SatelliteClient::kAckMotionFlagSinkSupportedForType == 0x01);
    REQUIRE(SatelliteClient::kAckMotionFlagBackendOk == 0x02);
    REQUIRE((SatelliteClient::kAckMotionFlagSinkSupportedForType &
             SatelliteClient::kAckMotionFlagBackendOk) == 0);
}
