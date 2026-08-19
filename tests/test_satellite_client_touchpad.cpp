// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

// MSG_TOUCHPAD (0x000C) inner payload, matching satellite core/types.h
// TouchpadReport: ctrlIdx(1) flags(1), then per finger id(1) x(2 LE) y(2 LE),
// then eventTimeMs(4 LE). flags bit 0 = finger0 active, bit 1 = finger1 active,
// bit 2 = button. The server drops an inner payload shorter than 16 bytes.

#include "Network/SatelliteClient.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>

using dish::net::SatelliteClient;

namespace {

std::int16_t readLe16(const std::uint8_t* p) {
    return static_cast<std::int16_t>(static_cast<std::uint16_t>(p[0]) |
                                     (static_cast<std::uint16_t>(p[1]) << 8));
}

std::uint32_t readLe32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

} // namespace

TEST_CASE("encodeTouchpadPayload is 16 bytes with ctrlIdx at byte 0", "[touchpad]") {
    const auto out = SatelliteClient::encodeTouchpadPayload(
        /*ctrlIdx=*/6, /*f0Active=*/false, /*f0Id=*/0, /*f0X=*/0, /*f0Y=*/0,
        /*f1Active=*/false, /*f1Id=*/0, /*f1X=*/0, /*f1Y=*/0, /*button=*/false, /*eventTimeMs=*/0);
    REQUIRE(out.size() == 16U);
    REQUIRE(out[0] == 6U);
}

TEST_CASE("encodeTouchpadPayload flags: all inactive, no button -> 0", "[touchpad]") {
    const auto out =
        SatelliteClient::encodeTouchpadPayload(0, false, 0, 0, 0, false, 0, 0, 0, false, 0);
    REQUIRE(out[1] == 0x00U);
}

TEST_CASE("encodeTouchpadPayload flags: bit 0 = finger0 active", "[touchpad]") {
    const auto out = SatelliteClient::encodeTouchpadPayload(0, /*f0Active=*/true, 0, 0, 0, false, 0,
                                                            0, 0, false, 0);
    REQUIRE(out[1] == 0x01U);
}

TEST_CASE("encodeTouchpadPayload flags: bit 1 = finger1 active", "[touchpad]") {
    const auto out = SatelliteClient::encodeTouchpadPayload(0, false, 0, 0, 0, /*f1Active=*/true, 0,
                                                            0, 0, false, 0);
    REQUIRE(out[1] == 0x02U);
}

TEST_CASE("encodeTouchpadPayload flags: bit 2 = button pressed", "[touchpad]") {
    const auto out = SatelliteClient::encodeTouchpadPayload(0, false, 0, 0, 0, false, 0, 0, 0,
                                                            /*button=*/true, 0);
    REQUIRE(out[1] == 0x04U);
}

TEST_CASE("encodeTouchpadPayload flags: all three bits set together -> 0x07", "[touchpad]") {
    const auto out = SatelliteClient::encodeTouchpadPayload(0, /*f0Active=*/true, 0, 0, 0,
                                                            /*f1Active=*/true, 0, 0, 0,
                                                            /*button=*/true, 0);
    REQUIRE(out[1] == 0x07U);
}

TEST_CASE("encodeTouchpadPayload places finger0 id/x/y at bytes 2,3,5", "[touchpad]") {
    const auto out = SatelliteClient::encodeTouchpadPayload(
        /*ctrlIdx=*/0, /*f0Active=*/true, /*f0Id=*/0x2A, /*f0X=*/0x1234, /*f0Y=*/0x5678,
        /*f1Active=*/false, /*f1Id=*/0, /*f1X=*/0, /*f1Y=*/0, /*button=*/false, /*eventTimeMs=*/0);
    REQUIRE(out[2] == 0x2AU);
    REQUIRE(readLe16(&out[3]) == 0x1234);
    REQUIRE(readLe16(&out[5]) == 0x5678);
}

TEST_CASE("encodeTouchpadPayload places finger1 id/x/y at bytes 7,8,10", "[touchpad]") {
    const auto out = SatelliteClient::encodeTouchpadPayload(
        /*ctrlIdx=*/0, /*f0Active=*/false, /*f0Id=*/0, /*f0X=*/0, /*f0Y=*/0,
        /*f1Active=*/true, /*f1Id=*/0x3B, /*f1X=*/0x09AB, /*f1Y=*/0x0CDE, /*button=*/false,
        /*eventTimeMs=*/0);
    REQUIRE(out[7] == 0x3BU);
    REQUIRE(readLe16(&out[8]) == 0x09AB);
    REQUIRE(readLe16(&out[10]) == 0x0CDE);
}

TEST_CASE("encodeTouchpadPayload appends eventTimeMs as u32 LE at bytes 12..15", "[touchpad]") {
    // The byte order guards interop with the satellite's decodeTouchpadReport,
    // which reads le32(p + 11).
    const auto out = SatelliteClient::encodeTouchpadPayload(
        /*ctrlIdx=*/1, /*f0Active=*/true, /*f0Id=*/0, /*f0X=*/0, /*f0Y=*/0,
        /*f1Active=*/false, /*f1Id=*/0, /*f1X=*/0, /*f1Y=*/0, /*button=*/false,
        /*eventTimeMs=*/0x01020304U);
    REQUIRE(out[12] == 0x04U);
    REQUIRE(out[13] == 0x03U);
    REQUIRE(out[14] == 0x02U);
    REQUIRE(out[15] == 0x01U);
    REQUIRE(readLe32(&out[12]) == 0x01020304U);
}

TEST_CASE("encodeTouchpadPayload eventTimeMs spans the full u32 range", "[touchpad]") {
    const auto out =
        SatelliteClient::encodeTouchpadPayload(0, false, 0, 0, 0, false, 0, 0, 0, false,
                                               /*eventTimeMs=*/0xFFFFFFFFU);
    REQUIRE(readLe32(&out[12]) == 0xFFFFFFFFU);
}

TEST_CASE("encodeTouchpadPayload writes coordinates as host-LE int16", "[touchpad]") {
    const auto out =
        SatelliteClient::encodeTouchpadPayload(0, true, 0, /*f0X=*/0x0102,
                                               /*f0Y=*/0x0304, true, 0,
                                               /*f1X=*/0x0506, /*f1Y=*/0x0708, false, 0);
    REQUIRE(out[3] == 0x02U);
    REQUIRE(out[4] == 0x01U);
    REQUIRE(out[5] == 0x04U);
    REQUIRE(out[6] == 0x03U);
    REQUIRE(out[8] == 0x06U);
    REQUIRE(out[9] == 0x05U);
    REQUIRE(out[10] == 0x08U);
    REQUIRE(out[11] == 0x07U);
}

TEST_CASE("encodeTouchpadPayload handles negative coordinates and full range", "[touchpad]") {
    const auto out = SatelliteClient::encodeTouchpadPayload(
        /*ctrlIdx=*/0xFF, /*f0Active=*/true, /*f0Id=*/0xFF, /*f0X=*/-32768, /*f0Y=*/32767,
        /*f1Active=*/true, /*f1Id=*/0xFF, /*f1X=*/-1, /*f1Y=*/0, /*button=*/true,
        /*eventTimeMs=*/0);
    REQUIRE(out[0] == 0xFFU);
    REQUIRE(out[2] == 0xFFU);
    REQUIRE(readLe16(&out[3]) == -32768);
    REQUIRE(readLe16(&out[5]) == 32767);
    REQUIRE(out[7] == 0xFFU);
    REQUIRE(readLe16(&out[8]) == -1);
    REQUIRE(readLe16(&out[10]) == 0);
}

TEST_CASE("encodeTouchpadPayload still encodes id/coords for inactive fingers", "[touchpad]") {
    // The encoder never zeroes an inactive finger's id/coordinates, so the
    // flags bits are the sole source of truth.
    const auto out = SatelliteClient::encodeTouchpadPayload(
        /*ctrlIdx=*/1, /*f0Active=*/false, /*f0Id=*/0x11, /*f0X=*/0x2222, /*f0Y=*/0x3333,
        /*f1Active=*/false, /*f1Id=*/0x44, /*f1X=*/0x5555, /*f1Y=*/0x6666, /*button=*/false,
        /*eventTimeMs=*/0x77777777U);
    REQUIRE(out[1] == 0x00U);
    REQUIRE(out[2] == 0x11U);
    REQUIRE(readLe16(&out[3]) == 0x2222);
    REQUIRE(readLe16(&out[5]) == 0x3333);
    REQUIRE(out[7] == 0x44U);
    REQUIRE(readLe16(&out[8]) == 0x5555);
    REQUIRE(readLe16(&out[10]) == 0x6666);
    REQUIRE(readLe32(&out[12]) == 0x77777777U);
}
