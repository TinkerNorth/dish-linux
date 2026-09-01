// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The protocol-2 POINTER frame on opcode 0x000C, and the protocol-2 feedback
// return paths (TRIGGER_EFFECTS 0x0010, PLAYER_LEDS 0x0011).
//
// The v1 frame is pinned next door in test_satellite_client_touchpad.cpp. What
// this file adds is the reshape: the click moved OUT of the finger flags into a
// buttons byte, everything after it shifted by one, and a signed wheel was
// appended. A decoder keyed on frame length is what lets the two coexist, so the
// lengths are load-bearing and asserted first.

#include "Network/SatelliteClient.h"
#include "core/model/Protocol.h"
#include "core/reducer/TouchpadRouting.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>

using dish::net::SatelliteClient;
using dish::reducer::assembleTouchpadForward;
using dish::reducer::pointerButtons;
using dish::reducer::pointerFingerFlags;
using dish::reducer::TouchpadForward;

namespace proto = dish::proto;

namespace {

std::int16_t readLe16(const std::uint8_t* p) {
    return static_cast<std::int16_t>(static_cast<std::uint16_t>(p[0]) |
                                     (static_cast<std::uint16_t>(p[1]) << 8));
}

std::uint32_t readLe32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

TouchpadForward twoFingers() {
    return assembleTouchpadForward(/*f0Active=*/true, /*f0Id=*/0x11, /*f0X=*/-1000, /*f0Y=*/2000,
                                   /*f1Active=*/true, /*f1Id=*/0x22, /*f1X=*/3000, /*f1Y=*/-4000,
                                   /*button=*/false, /*eventTimeMs=*/0x01020304);
}

} // namespace

TEST_CASE("the POINTER frame is 19 bytes, three longer than the v1 frame", "[pointer]") {
    // The receiver picks its decoder by length, so these two numbers ARE the
    // discriminator. 16 and 19 must stay distinct and stay what the contract says.
    const auto v1 =
        SatelliteClient::encodeTouchpadPayload(0, false, 0, 0, 0, false, 0, 0, 0, false, 0);
    const auto v2 = SatelliteClient::encodePointerPayload(0, TouchpadForward{});
    CHECK(v1.size() == 16U);
    CHECK(v2.size() == 19U);
    CHECK(static_cast<int>(v1.size()) == 1 + proto::kTouchpadPayloadBytes);
    CHECK(static_cast<int>(v2.size()) == 1 + proto::kPointerPayloadBytes);
}

TEST_CASE("POINTER lays its fields out at the shifted offsets", "[pointer]") {
    const auto out = SatelliteClient::encodePointerPayload(/*ctrlIdx=*/9, twoFingers());
    REQUIRE(out.size() == 19U);
    CHECK(out[0] == 9U);
    CHECK(out[1] == 0x03U); // both fingers
    CHECK(out[2] == 0x00U); // no buttons
    // Each finger sits one byte later than in v1, because of the buttons byte.
    CHECK(out[3] == 0x11U);
    CHECK(readLe16(&out[4]) == -1000);
    CHECK(readLe16(&out[6]) == 2000);
    CHECK(out[8] == 0x22U);
    CHECK(readLe16(&out[9]) == 3000);
    CHECK(readLe16(&out[11]) == -4000);
    CHECK(readLe32(&out[13]) == 0x01020304U);
    CHECK(readLe16(&out[17]) == 0);
}

TEST_CASE("the click is a button now, not a finger flag", "[pointer]") {
    // The one behaviour change of the reshape. A v2 encoder that still set
    // flags bit 2 would tell the host a third finger is down.
    TouchpadForward f;
    f.buttonPressed = true;
    const auto out = SatelliteClient::encodePointerPayload(0, f);
    CHECK(out[1] == 0x00U);                     // finger flags untouched
    CHECK(out[2] == proto::kPointerButtonLeft); // ...and the click is here
    CHECK((out[1] & 0x04U) == 0U);              // v1's click bit is not set
}

TEST_CASE("finger flags carry only the two fingers", "[pointer]") {
    TouchpadForward f;
    CHECK(pointerFingerFlags(f) == 0x00U);
    f.finger0Active = true;
    CHECK(pointerFingerFlags(f) == proto::kPointerFinger0Active);
    f.finger1Active = true;
    CHECK(pointerFingerFlags(f) == (proto::kPointerFinger0Active | proto::kPointerFinger1Active));
    // A pressed click still does not appear in this byte.
    f.buttonPressed = true;
    CHECK(pointerFingerFlags(f) == (proto::kPointerFinger0Active | proto::kPointerFinger1Active));
}

TEST_CASE("the buttons byte packs left, right and middle in order", "[pointer]") {
    TouchpadForward f;
    CHECK(pointerButtons(f) == 0x00U);
    f.buttonPressed = true;
    CHECK(pointerButtons(f) == 0x01U);
    f.rightPressed = true;
    CHECK(pointerButtons(f) == 0x03U);
    f.middlePressed = true;
    CHECK(pointerButtons(f) == 0x07U);
    f.buttonPressed = false;
    CHECK(pointerButtons(f) == 0x06U);
}

TEST_CASE("the wheel is signed and rides the last two bytes", "[pointer]") {
    TouchpadForward f;
    f.scrollV = proto::kScrollUnitsPerNotch;
    auto out = SatelliteClient::encodePointerPayload(0, f);
    CHECK(readLe16(&out[17]) == 120);
    f.scrollV = static_cast<std::int16_t>(-proto::kScrollUnitsPerNotch);
    out = SatelliteClient::encodePointerPayload(0, f);
    CHECK(readLe16(&out[17]) == -120);
}

TEST_CASE("a physical pad's touchpad produces no right, middle or wheel", "[pointer]") {
    // This client has no on-screen trackpad, so the only producer is a pad's own
    // touch surface, which reports one click and nothing else. Synthesising a
    // two-finger right-click here would send the host a button the user never
    // pressed. assembleTouchpadForward is the whole of that producer's vocabulary.
    const auto f = assembleTouchpadForward(true, 1, 10, 20, false, 0, 0, 0, /*button=*/true, 5);
    CHECK_FALSE(f.rightPressed);
    CHECK_FALSE(f.middlePressed);
    CHECK(f.scrollV == 0);
    const auto out = SatelliteClient::encodePointerPayload(0, f);
    CHECK(out[2] == proto::kPointerButtonLeft);
    CHECK(readLe16(&out[17]) == 0);
}

TEST_CASE("an inactive finger's stale coordinates still ride the frame", "[pointer]") {
    // Same contract as v1: the active flags are the sole source of truth, so the
    // encoder does not zero a released finger's slot. Pinned because "tidying"
    // it up would be an easy, wrong change.
    TouchpadForward f;
    f.finger0Active = false;
    f.finger0Id = 0x5A;
    f.finger0X = 1234;
    const auto out = SatelliteClient::encodePointerPayload(0, f);
    CHECK(out[1] == 0x00U);
    CHECK(out[3] == 0x5AU);
    CHECK(readLe16(&out[4]) == 1234);
}

// ── The feedback return paths ────────────────────────────────────────────────

TEST_CASE("TRIGGER_EFFECTS parses 23 bytes into two verbatim blocks", "[feedback][wire]") {
    std::array<std::uint8_t, 23> payload{};
    payload[0] = 3; // ctrlIdx
    for (std::size_t i = 0; i < 11; ++i) {
        payload[1 + i] = static_cast<std::uint8_t>(0xA0 + i);  // left
        payload[12 + i] = static_cast<std::uint8_t>(0xB0 + i); // right
    }
    const auto msg = SatelliteClient::parseTriggerEffectsMessage(payload.data(), payload.size());
    REQUIRE(msg.has_value());
    CHECK(msg->controllerIndex == 3);
    for (std::size_t i = 0; i < 11; ++i) {
        CHECK(msg->left[i] == static_cast<std::uint8_t>(0xA0 + i));
        CHECK(msg->right[i] == static_cast<std::uint8_t>(0xB0 + i));
    }
}

TEST_CASE("a short TRIGGER_EFFECTS frame is dropped, not padded", "[feedback][wire]") {
    // These bytes are replayed verbatim into firmware, so a partial frame must
    // never become a report with uninitialised tail bytes.
    std::array<std::uint8_t, 23> payload{};
    for (std::size_t len = 0; len < payload.size(); ++len) {
        INFO("len " << len);
        CHECK_FALSE(SatelliteClient::parseTriggerEffectsMessage(payload.data(), len).has_value());
    }
    CHECK(SatelliteClient::parseTriggerEffectsMessage(payload.data(), payload.size()).has_value());
    CHECK_FALSE(SatelliteClient::parseTriggerEffectsMessage(nullptr, payload.size()).has_value());
}

TEST_CASE("a longer TRIGGER_EFFECTS frame still parses its first 23 bytes", "[feedback][wire]") {
    // Forward-compatibility: a newer server appending a field must not make the
    // whole message undecodable here.
    std::array<std::uint8_t, 40> payload{};
    payload[0] = 1;
    payload[1] = 0x42;
    const auto msg = SatelliteClient::parseTriggerEffectsMessage(payload.data(), payload.size());
    REQUIRE(msg.has_value());
    CHECK(msg->controllerIndex == 1);
    CHECK(msg->left[0] == 0x42);
}

TEST_CASE("PLAYER_LEDS parses ctrlIdx and the mask", "[feedback][wire]") {
    const std::array<std::uint8_t, 2> payload{7, 0x1F};
    const auto msg = SatelliteClient::parsePlayerLedsMessage(payload.data(), payload.size());
    REQUIRE(msg.has_value());
    CHECK(msg->controllerIndex == 7);
    CHECK(msg->ledMask == 0x1F);
}

TEST_CASE("the LED mask is passed through unmasked", "[feedback][wire]") {
    // Masking to a family's real LEDs is the report builder's job, because only
    // it knows the family. Doing it here would silently drop a bit a future pad
    // does have.
    const std::array<std::uint8_t, 2> payload{0, 0xFF};
    const auto msg = SatelliteClient::parsePlayerLedsMessage(payload.data(), payload.size());
    REQUIRE(msg.has_value());
    CHECK(msg->ledMask == 0xFF);
}

TEST_CASE("a short PLAYER_LEDS frame is dropped", "[feedback][wire]") {
    const std::array<std::uint8_t, 2> payload{1, 1};
    CHECK_FALSE(SatelliteClient::parsePlayerLedsMessage(payload.data(), 0).has_value());
    CHECK_FALSE(SatelliteClient::parsePlayerLedsMessage(payload.data(), 1).has_value());
    CHECK_FALSE(SatelliteClient::parsePlayerLedsMessage(nullptr, 2).has_value());
}

TEST_CASE("the protocol-2 opcodes and caps are the contract's values", "[feedback][wire]") {
    // Deleted opcodes stay retired and the new ones start at 0x0010; the caps
    // bits are what the satellite gates its sends on.
    CHECK(SatelliteClient::kMsgTriggerEffects == 0x0010);
    CHECK(SatelliteClient::kMsgPlayerLeds == 0x0011);
    CHECK(SatelliteClient::kCapTriggerEffects == 0x0010);
    CHECK(SatelliteClient::kCapPlayerLeds == 0x0020);
    // ...and they do not collide with the caps that already existed.
    CHECK((SatelliteClient::kCapTriggerEffects & SatelliteClient::kCapLightbar) == 0);
    CHECK((SatelliteClient::kCapPlayerLeds & SatelliteClient::kCapLightbar) == 0);
    CHECK((SatelliteClient::kCapTriggerEffects & SatelliteClient::kCapPlayerLeds) == 0);
}

TEST_CASE("the caps helpers fold only their own bit", "[feedback][wire]") {
    const std::uint16_t base = SatelliteClient::kCapAnalogTriggers;
    CHECK(SatelliteClient::withTriggerEffectsCapability(base, false) == base);
    CHECK(SatelliteClient::withTriggerEffectsCapability(base, true) ==
          (base | SatelliteClient::kCapTriggerEffects));
    CHECK(SatelliteClient::withPlayerLedsCapability(base, false) == base);
    CHECK(SatelliteClient::withPlayerLedsCapability(base, true) ==
          (base | SatelliteClient::kCapPlayerLeds));
    // Folding both keeps both.
    const auto both = SatelliteClient::withPlayerLedsCapability(
        SatelliteClient::withTriggerEffectsCapability(base, true), true);
    CHECK((both & SatelliteClient::kCapTriggerEffects) != 0);
    CHECK((both & SatelliteClient::kCapPlayerLeds) != 0);
    CHECK((both & SatelliteClient::kCapAnalogTriggers) != 0);
}
