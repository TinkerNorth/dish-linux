// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// MSG_INPUT (0x0001) by the settled version, the client half of the contract's
// "Extended buttons": ctrlIdx(1) + the 12-byte XUSB report on a session settled
// at 1..3, and the same 13 bytes followed by the extended-buttons word (u32 LE)
// on one settled at 4. The receiver reads the frame by length, so the two sizes
// are the discriminator and are asserted first.

#include "Network/SatelliteClient.h"
#include "core/model/Protocol.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

using dish::net::SatelliteClient;
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

constexpr std::uint8_t kCtrlIdx = 3;
constexpr std::uint16_t kButtons = 0x1234;
constexpr std::uint8_t kLt = 0x56;
constexpr std::uint8_t kRt = 0xFE;
constexpr std::int16_t kLx = -12345;
constexpr std::int16_t kLy = 23456;
constexpr std::int16_t kRx = -32768;
constexpr std::int16_t kRy = 32767;
constexpr std::uint32_t kWord = 0x1003;

constexpr std::size_t kThirteen = 13;
constexpr std::size_t kSeventeen = 17;
constexpr std::size_t kWordOffset = 13;

SatelliteClient::InputFrame frameAt(int settledVersion, std::uint32_t extButtons) {
    return SatelliteClient::encodeInputPayload(settledVersion, kCtrlIdx, kButtons, kLt, kRt, kLx,
                                               kLy, kRx, kRy, extButtons);
}

bool sameFirstThirteen(const SatelliteClient::InputFrame& a, const SatelliteClient::InputFrame& b) {
    return std::equal(a.bytes.begin(), a.bytes.begin() + kThirteen, b.bytes.begin());
}

} // namespace

TEST_CASE("INPUT is 13 bytes on a session settled before protocol 4 and 17 bytes from it",
          "[input][wire]") {
    CHECK(frameAt(1, kWord).length == kThirteen);
    CHECK(frameAt(2, kWord).length == kThirteen);
    CHECK(frameAt(3, kWord).length == kThirteen);
    CHECK(frameAt(4, kWord).length == kSeventeen);
    CHECK(kThirteen == 1 + static_cast<std::size_t>(proto::kInputPayloadBytes));
    CHECK(kSeventeen == 1 + static_cast<std::size_t>(proto::kInputExtendedPayloadBytes));
    CHECK(SatelliteClient::kInputFrameMaxBytes == kSeventeen);
}

TEST_CASE("the report keeps its protocol 1 layout at every settled version", "[input][wire]") {
    for (const int version : {1, 2, 3, 4}) {
        const auto f = frameAt(version, kWord);
        CHECK(f.bytes[0] == kCtrlIdx);
        CHECK(f.bytes[1] == 0x34U);
        CHECK(f.bytes[2] == 0x12U);
        CHECK(f.bytes[3] == kLt);
        CHECK(f.bytes[4] == kRt);
        CHECK(readLe16(&f.bytes[5]) == kLx);
        CHECK(readLe16(&f.bytes[7]) == kLy);
        CHECK(readLe16(&f.bytes[9]) == kRx);
        CHECK(readLe16(&f.bytes[11]) == kRy);
    }
}

TEST_CASE("the extended-buttons word rides little-endian behind the report from protocol 4",
          "[input][wire]") {
    const auto f = frameAt(4, kWord);
    CHECK(f.bytes[kWordOffset] == 0x03U);
    CHECK(f.bytes[kWordOffset + 1] == 0x10U);
    CHECK(f.bytes[kWordOffset + 2] == 0x00U);
    CHECK(f.bytes[kWordOffset + 3] == 0x00U);
    CHECK(readLe32(&f.bytes[kWordOffset]) == kWord);
}

TEST_CASE("a session before protocol 4 is sent the thirteen bytes whatever the word",
          "[input][wire]") {
    const auto held = frameAt(3, kWord);
    const auto clear = frameAt(3, 0);
    CHECK(held.length == kThirteen);
    CHECK(sameFirstThirteen(held, clear));
    const auto atFour = frameAt(4, kWord);
    CHECK(sameFirstThirteen(held, atFour));
}

TEST_CASE("the reserved bits of the word are written as zero", "[input][wire]") {
    const auto f = frameAt(4, 0xFFFFFFFFU);
    CHECK(readLe32(&f.bytes[kWordOffset]) == proto::kExtButtonsKnownMask);
    CHECK(proto::kExtButtonsKnownMask == 0x1FFFU);
}

TEST_CASE("the extended-buttons bits are the contract table, in SDL's names", "[input][wire]") {
    CHECK(proto::kExtButtonRightPaddle1 == 0x0001U);
    CHECK(proto::kExtButtonLeftPaddle1 == 0x0002U);
    CHECK(proto::kExtButtonRightPaddle2 == 0x0004U);
    CHECK(proto::kExtButtonLeftPaddle2 == 0x0008U);
    CHECK(proto::kExtButtonMisc1 == 0x0010U);
    CHECK(proto::kExtButtonLeftPadClick == 0x0020U);
    CHECK(proto::kExtButtonRightPadClick == 0x0040U);
    CHECK(proto::kExtButtonLeftTriggerClick == 0x0080U);
    CHECK(proto::kExtButtonRightTriggerClick == 0x0100U);
    CHECK(proto::kExtButtonLeftStickTouch == 0x0200U);
    CHECK(proto::kExtButtonRightStickTouch == 0x0400U);
    CHECK(proto::kExtButtonLeftGripTouch == 0x0800U);
    CHECK(proto::kExtButtonRightGripTouch == 0x1000U);
    const std::uint32_t everyNamedBit =
        proto::kExtButtonRightPaddle1 | proto::kExtButtonLeftPaddle1 |
        proto::kExtButtonRightPaddle2 | proto::kExtButtonLeftPaddle2 | proto::kExtButtonMisc1 |
        proto::kExtButtonLeftPadClick | proto::kExtButtonRightPadClick |
        proto::kExtButtonLeftTriggerClick | proto::kExtButtonRightTriggerClick |
        proto::kExtButtonLeftStickTouch | proto::kExtButtonRightStickTouch |
        proto::kExtButtonLeftGripTouch | proto::kExtButtonRightGripTouch;
    CHECK(everyNamedBit == proto::kExtButtonsKnownMask);
}

TEST_CASE("the extended-buttons cap is 0x0200 and its helper folds only its bit", "[input][wire]") {
    CHECK(proto::kCapExtendedButtons == 0x0200U);
    CHECK(SatelliteClient::kCapExtendedButtons == proto::kCapExtendedButtons);
    const std::uint16_t prior = SatelliteClient::kCapAnalogTriggers | SatelliteClient::kCapRumble |
                                SatelliteClient::kCapMotion | SatelliteClient::kCapLightbar |
                                SatelliteClient::kCapTriggerEffects |
                                SatelliteClient::kCapPlayerLeds | SatelliteClient::kCapMic |
                                SatelliteClient::kCapSpeaker | SatelliteClient::kCapHapticAudio;
    CHECK((prior & SatelliteClient::kCapExtendedButtons) == 0);
    const std::uint16_t base = SatelliteClient::kCapAnalogTriggers;
    CHECK(SatelliteClient::withExtendedButtonsCapability(base, false) == base);
    CHECK(SatelliteClient::withExtendedButtonsCapability(base, true) ==
          (base | SatelliteClient::kCapExtendedButtons));
}
