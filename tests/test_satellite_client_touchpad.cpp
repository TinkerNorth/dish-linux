// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

// Coverage for SatelliteClient::encodeTouchpadPayload — the pure encoder for
// the MSG_TOUCHPAD (0x000C) inner payload (DualSense / DS4 two-finger pad).
// The 16-byte protocol-1 layout must match satellite/src/core/types.h::
// TouchpadReport byte-for-byte. Same pattern as
// test_satellite_client_motion.cpp — the encoder is public + static so we can
// pin the byte order without driving a live socket.
//
// Wire layout (16 bytes):
//   ctrlIdx(1) flags(1) f0[id(1) x(2 LE) y(2 LE)] f1[id(1) x(2 LE) y(2 LE)]
//   eventTimeMs(4 LE)
//   flags bit 0 = finger0 active, bit 1 = finger1 active, bit 2 = button.
//   The trailing eventTimeMs is the protocol-1 addition (was 12 bytes); the
//   server drops inner payloads shorter than 16.

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

} // namespace

TEST_CASE("encodeTouchpadPayload places ctrlIdx at byte 0", "[touchpad]") {
    const auto out = SatelliteClient::encodeTouchpadPayload(
        /*ctrlIdx=*/9, false, 0, 0, 0, false, 0, 0, 0, false, 0);
    REQUIRE(out.size() == 16U);
    REQUIRE(out[0] == 9U);
}

TEST_CASE("encodeTouchpadPayload flags byte encodes finger0 / finger1 / button bits",
          "[touchpad]") {
    SECTION("all clear") {
        const auto out =
            SatelliteClient::encodeTouchpadPayload(0, false, 0, 0, 0, false, 0, 0, 0, false, 0);
        REQUIRE(out[1] == 0x00U);
    }
    SECTION("finger0 only -> bit 0") {
        const auto out =
            SatelliteClient::encodeTouchpadPayload(0, true, 0, 0, 0, false, 0, 0, 0, false, 0);
        REQUIRE(out[1] == 0x01U);
    }
    SECTION("finger1 only -> bit 1") {
        const auto out =
            SatelliteClient::encodeTouchpadPayload(0, false, 0, 0, 0, true, 0, 0, 0, false, 0);
        REQUIRE(out[1] == 0x02U);
    }
    SECTION("button only -> bit 2") {
        const auto out =
            SatelliteClient::encodeTouchpadPayload(0, false, 0, 0, 0, false, 0, 0, 0, true, 0);
        REQUIRE(out[1] == 0x04U);
    }
    SECTION("all set -> 0x07") {
        const auto out =
            SatelliteClient::encodeTouchpadPayload(0, true, 0, 0, 0, true, 0, 0, 0, true, 0);
        REQUIRE(out[1] == 0x07U);
    }
}

TEST_CASE("encodeTouchpadPayload lays out finger0 id + LE coordinates", "[touchpad]") {
    // finger0: id@2, x LE@3, y LE@5.
    const auto out = SatelliteClient::encodeTouchpadPayload(
        /*ctrlIdx=*/0, /*f0Active=*/true, /*f0Id=*/0x2A, /*f0X=*/0x1234, /*f0Y=*/0x5678,
        /*f1Active=*/false, /*f1Id=*/0, /*f1X=*/0, /*f1Y=*/0, /*button=*/false, /*eventTimeMs=*/0);
    REQUIRE(out[2] == 0x2AU);
    REQUIRE(readLe16(&out[3]) == 0x1234);
    REQUIRE(readLe16(&out[5]) == 0x5678);
}

TEST_CASE("encodeTouchpadPayload lays out finger1 id + LE coordinates", "[touchpad]") {
    // finger1: id@7, x LE@8, y LE@10.
    const auto out = SatelliteClient::encodeTouchpadPayload(
        /*ctrlIdx=*/0, /*f0Active=*/false, /*f0Id=*/0, /*f0X=*/0, /*f0Y=*/0,
        /*f1Active=*/true, /*f1Id=*/0x55, /*f1X=*/0x0A0B, /*f1Y=*/0x0C0D, /*button=*/false,
        /*eventTimeMs=*/0);
    REQUIRE(out[7] == 0x55U);
    REQUIRE(readLe16(&out[8]) == 0x0A0B);
    REQUIRE(readLe16(&out[10]) == 0x0C0D);
}

TEST_CASE("encodeTouchpadPayload writes a full two-finger frame in order", "[touchpad]") {
    const auto out = SatelliteClient::encodeTouchpadPayload(
        /*ctrlIdx=*/3, /*f0Active=*/true, /*f0Id=*/1, /*f0X=*/100, /*f0Y=*/-200,
        /*f1Active=*/true, /*f1Id=*/2, /*f1X=*/-300, /*f1Y=*/400, /*button=*/true,
        /*eventTimeMs=*/0);
    REQUIRE(out[0] == 3U);
    REQUIRE(out[1] == 0x07U); // f0 + f1 + button
    REQUIRE(out[2] == 1U);
    REQUIRE(readLe16(&out[3]) == 100);
    REQUIRE(readLe16(&out[5]) == -200);
    REQUIRE(out[7] == 2U);
    REQUIRE(readLe16(&out[8]) == -300);
    REQUIRE(readLe16(&out[10]) == 400);
}

TEST_CASE("encodeTouchpadPayload handles full int16 coordinate range without overflow",
          "[touchpad]") {
    const auto out = SatelliteClient::encodeTouchpadPayload(
        /*ctrlIdx=*/0xFF, /*f0Active=*/true, /*f0Id=*/0xFF, /*f0X=*/-32768, /*f0Y=*/32767,
        /*f1Active=*/true, /*f1Id=*/0xFF, /*f1X=*/32767, /*f1Y=*/-32768, /*button=*/false,
        /*eventTimeMs=*/0);
    REQUIRE(out[0] == 0xFFU);
    REQUIRE(out[2] == 0xFFU);
    REQUIRE(readLe16(&out[3]) == -32768);
    REQUIRE(readLe16(&out[5]) == 32767);
    REQUIRE(out[7] == 0xFFU);
    REQUIRE(readLe16(&out[8]) == 32767);
    REQUIRE(readLe16(&out[10]) == -32768);
}

TEST_CASE("encodeTouchpadPayload still encodes coordinates for an inactive finger", "[touchpad]") {
    // The encoder writes the id + coordinate bytes unconditionally; only the
    // flags byte records activity. The receiver gates on flags, so stale
    // coordinates behind a cleared bit are harmless — pin that the bytes are
    // still laid out (no short payload) when a finger is inactive.
    const auto out = SatelliteClient::encodeTouchpadPayload(
        /*ctrlIdx=*/0, /*f0Active=*/false, /*f0Id=*/7, /*f0X=*/111, /*f0Y=*/222,
        /*f1Active=*/false, /*f1Id=*/8, /*f1X=*/333, /*f1Y=*/444, /*button=*/false,
        /*eventTimeMs=*/0);
    REQUIRE(out.size() == 16U);
    REQUIRE(out[1] == 0x00U);
    REQUIRE(out[2] == 7U);
    REQUIRE(readLe16(&out[3]) == 111);
    REQUIRE(readLe16(&out[5]) == 222);
    REQUIRE(out[7] == 8U);
    REQUIRE(readLe16(&out[8]) == 333);
    REQUIRE(readLe16(&out[10]) == 444);
}

TEST_CASE("encodeTouchpadPayload appends eventTimeMs as trailing LE u32", "[touchpad]") {
    const auto out = SatelliteClient::encodeTouchpadPayload(
        /*ctrlIdx=*/0, /*f0Active=*/true, /*f0Id=*/1, /*f0X=*/0, /*f0Y=*/0,
        /*f1Active=*/false, /*f1Id=*/0, /*f1X=*/0, /*f1Y=*/0, /*button=*/false,
        /*eventTimeMs=*/0xAABBCCDDU);
    REQUIRE(out.size() == 16U);
    REQUIRE(out[12] == 0xDDU);
    REQUIRE(out[13] == 0xCCU);
    REQUIRE(out[14] == 0xBBU);
    REQUIRE(out[15] == 0xAAU);
}

TEST_CASE("encodeTouchpadPayload matches proto::kTouchpadPayloadBytes + ctrlIdx", "[touchpad]") {
    const auto out =
        SatelliteClient::encodeTouchpadPayload(0, false, 0, 0, 0, false, 0, 0, 0, false, 0);
    REQUIRE(out.size() == static_cast<std::size_t>(dish::proto::kTouchpadPayloadBytes) + 1U);
}
