// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

// Coverage for SatelliteClient::encodeMotionPayload / encodeBatteryPayload —
// the pure encoders for MSG_MOTION (0x000A) and MSG_BATTERY (0x000B). The
// wire layout these produce must match satellite/src/core/types.h::MotionReport
// and BatteryReport byte-for-byte (the receiver decodes via memcpy onto the
// host-LE struct). Same pattern as test_satellite_client_rumble.cpp — the
// encoders are public + static so we can pin the byte order without driving
// a live socket.

#include "Network/SatelliteClient.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>

using dish::net::SatelliteClient;

namespace {

// Helper: pull a host-LE int16 back out of a byte buffer for assertions.
std::int16_t readLe16(const std::uint8_t* p) {
    return static_cast<std::int16_t>(static_cast<std::uint16_t>(p[0]) |
                                     (static_cast<std::uint16_t>(p[1]) << 8));
}

std::uint32_t readLe32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

} // namespace

TEST_CASE("encodeMotionPayload places ctrlIdx at byte 0", "[motion]") {
    const auto out = SatelliteClient::encodeMotionPayload(/*ctrlIdx=*/7, 0, 0, 0, 0, 0, 0, 0);
    REQUIRE(out.size() == 17U);
    REQUIRE(out[0] == 7U);
}

TEST_CASE("encodeMotionPayload writes gyro / accel as LE int16", "[motion]") {
    const auto out = SatelliteClient::encodeMotionPayload(
        /*ctrlIdx=*/0, /*gx=*/0x0102, /*gy=*/0x0304, /*gz=*/0x0506,
        /*ax=*/0x0708, /*ay=*/0x090A, /*az=*/0x0B0C, /*dtUs=*/0);

    REQUIRE(readLe16(&out[1]) == 0x0102);
    REQUIRE(readLe16(&out[3]) == 0x0304);
    REQUIRE(readLe16(&out[5]) == 0x0506);
    REQUIRE(readLe16(&out[7]) == 0x0708);
    REQUIRE(readLe16(&out[9]) == 0x090A);
    REQUIRE(readLe16(&out[11]) == 0x0B0C);
}

TEST_CASE("encodeMotionPayload writes timestampDeltaUs as LE uint32", "[motion]") {
    const auto out = SatelliteClient::encodeMotionPayload(
        0, 0, 0, 0, 0, 0, 0, /*dtUs=*/0xDEADBEEFU);
    REQUIRE(readLe32(&out[13]) == 0xDEADBEEFU);
}

TEST_CASE("encodeMotionPayload handles full int16 range without overflow", "[motion]") {
    const auto out = SatelliteClient::encodeMotionPayload(
        0xFF, /*gx=*/-32768, /*gy=*/32767, /*gz=*/0,
        /*ax=*/-32768, /*ay=*/32767, /*az=*/-1, /*dtUs=*/0);
    REQUIRE(out[0] == 0xFFU);
    REQUIRE(readLe16(&out[1]) == -32768);
    REQUIRE(readLe16(&out[3]) == 32767);
    REQUIRE(readLe16(&out[7]) == -32768);
    REQUIRE(readLe16(&out[9]) == 32767);
    REQUIRE(readLe16(&out[11]) == -1);
}

TEST_CASE("encodeMotionPayload handles uint32 max delta", "[motion]") {
    const auto out = SatelliteClient::encodeMotionPayload(0, 0, 0, 0, 0, 0, 0, 0xFFFFFFFFU);
    REQUIRE(readLe32(&out[13]) == 0xFFFFFFFFU);
}

TEST_CASE("encodeMotionPayload zero-delta encodes as four zero bytes", "[motion]") {
    // First-packet sentinel — receiver expects exactly 0 (not 0xFFFFFFFF
    // or any other "no value" marker) so the inter-arrival timer can
    // recognise the start of a session.
    const auto out = SatelliteClient::encodeMotionPayload(0, 0, 0, 0, 0, 0, 0, 0);
    REQUIRE(out[13] == 0);
    REQUIRE(out[14] == 0);
    REQUIRE(out[15] == 0);
    REQUIRE(out[16] == 0);
}

TEST_CASE("encodeBatteryPayload writes ctrlIdx + level + status in order", "[battery]") {
    const auto out = SatelliteClient::encodeBatteryPayload(/*ctrlIdx=*/5, /*level=*/77,
                                                           /*status=*/2);
    REQUIRE(out.size() == 3U);
    REQUIRE(out[0] == 5U);
    REQUIRE(out[1] == 77U);
    REQUIRE(out[2] == 2U);
}

TEST_CASE("encodeBatteryPayload preserves 0xFF (unknown) sentinel", "[battery]") {
    const auto out = SatelliteClient::encodeBatteryPayload(0, 0xFF, 0);
    REQUIRE(out[1] == 0xFFU);
    REQUIRE(out[2] == 0U);
}

TEST_CASE("MSG constants match the wire-protocol spec (types.h)", "[constants]") {
    // These literal values are the contract between dish-windows and the
    // satellite receiver — bumping them silently would silently break
    // every paired client until they re-sync.
    REQUIRE(SatelliteClient::kMsgMotion == 0x000A);
    REQUIRE(SatelliteClient::kMsgBattery == 0x000B);
    REQUIRE(SatelliteClient::kCapMotion == 0x0004);
    REQUIRE(SatelliteClient::kBatteryLevelUnknown == 0xFF);
    REQUIRE(SatelliteClient::kBatteryStatusUnknown == 0);
    REQUIRE(SatelliteClient::kBatteryStatusDischarging == 1);
    REQUIRE(SatelliteClient::kBatteryStatusCharging == 2);
    REQUIRE(SatelliteClient::kBatteryStatusFull == 3);
    REQUIRE(SatelliteClient::kBatteryStatusWired == 4);
}
