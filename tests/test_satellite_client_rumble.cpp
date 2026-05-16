// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

// Coverage for SatelliteClient::parseRumbleMessage — the pure decoder for
// the satellite → dish MSG_RUMBLE payload (wire layout described in the
// declaration). The full I/O path (decrypt + dispatch) is intentionally
// out of scope for this test file; it would require driving the receive
// loop with a fake socket. The decoder is the only part that has its own
// branching logic worth pinning down with unit tests, and it's exposed
// publicly + statically for exactly that reason.

#include "Network/SatelliteClient.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <vector>

using dish::net::SatelliteClient;

namespace {

// Build the fixed 7-byte rumble payload. Matches the producer side in
// satellite/src/adapters/client_adapter.cpp::sendRumble.
std::array<std::uint8_t, 7> rumblePayload(std::uint8_t ctrlIdx, std::uint16_t strong,
                                          std::uint16_t weak, std::uint16_t dur) {
    return {
        ctrlIdx,
        static_cast<std::uint8_t>(strong >> 8),
        static_cast<std::uint8_t>(strong & 0xFF),
        static_cast<std::uint8_t>(weak >> 8),
        static_cast<std::uint8_t>(weak & 0xFF),
        static_cast<std::uint8_t>(dur >> 8),
        static_cast<std::uint8_t>(dur & 0xFF),
    };
}

} // namespace

TEST_CASE("parseRumbleMessage decodes the payload", "[rumble]") {
    auto p = rumblePayload(/*ctrlIdx=*/3, /*strong=*/0xABCD, /*weak=*/0x1234, /*dur=*/500);
    const auto rm = SatelliteClient::parseRumbleMessage(p.data(), p.size());
    REQUIRE(rm.has_value());
    REQUIRE(rm->controllerIndex == 3);
    REQUIRE(rm->strongMagnitude == 0xABCD);
    REQUIRE(rm->weakMagnitude == 0x1234);
    REQUIRE(rm->durationMs == 500);
}

TEST_CASE("parseRumbleMessage decodes a stop request (all zero magnitudes)", "[rumble]") {
    auto p = rumblePayload(/*ctrlIdx=*/0, /*strong=*/0, /*weak=*/0, /*dur=*/0);
    const auto rm = SatelliteClient::parseRumbleMessage(p.data(), p.size());
    REQUIRE(rm.has_value());
    REQUIRE(rm->strongMagnitude == 0);
    REQUIRE(rm->weakMagnitude == 0);
    REQUIRE(rm->durationMs == 0);
}

TEST_CASE("parseRumbleMessage decodes max-magnitude payload without overflow", "[rumble]") {
    auto p = rumblePayload(/*ctrlIdx=*/0xFF, /*strong=*/0xFFFF, /*weak=*/0xFFFF, /*dur=*/0xFFFF);
    const auto rm = SatelliteClient::parseRumbleMessage(p.data(), p.size());
    REQUIRE(rm.has_value());
    REQUIRE(rm->controllerIndex == 0xFF);
    REQUIRE(rm->strongMagnitude == 0xFFFF);
    REQUIRE(rm->weakMagnitude == 0xFFFF);
    REQUIRE(rm->durationMs == 0xFFFF);
}

TEST_CASE("parseRumbleMessage rejects a truncated payload", "[rumble]") {
    // Anything shorter than 7 bytes is malformed — the satellite never emits
    // such a packet, but a malicious / racing peer could.
    std::array<std::uint8_t, 6> shortPayload{};
    REQUIRE_FALSE(
        SatelliteClient::parseRumbleMessage(shortPayload.data(), shortPayload.size()).has_value());

    REQUIRE_FALSE(SatelliteClient::parseRumbleMessage(nullptr, 0).has_value());
    REQUIRE_FALSE(SatelliteClient::parseRumbleMessage(shortPayload.data(), 0).has_value());
}

TEST_CASE("parseRumbleMessage tolerates extra trailing bytes (forward-compat)", "[rumble]") {
    // Future protocol extensions may append fields after the fixed payload.
    // The decoder must return successfully and ignore the unknown bytes.
    std::vector<std::uint8_t> p(20, 0xAA);
    auto base = rumblePayload(2, 100, 50, 700);
    std::copy(base.begin(), base.end(), p.begin());
    const auto rm = SatelliteClient::parseRumbleMessage(p.data(), p.size());
    REQUIRE(rm.has_value());
    REQUIRE(rm->controllerIndex == 2);
    REQUIRE(rm->strongMagnitude == 100);
    REQUIRE(rm->weakMagnitude == 50);
    REQUIRE(rm->durationMs == 700);
}

TEST_CASE("parseRumbleMessage handles big-endian boundary cases", "[rumble]") {
    // Cover the byte-swap path: 0x00FF (low byte set), 0xFF00 (high byte set),
    // and 0x0100 (a value where naive little-endian read would be wrong).
    SECTION("low byte only") {
        auto p = rumblePayload(0, 0x00FF, 0x00FF, 0x00FF);
        const auto rm = SatelliteClient::parseRumbleMessage(p.data(), p.size());
        REQUIRE(rm.has_value());
        REQUIRE(rm->strongMagnitude == 0x00FF);
        REQUIRE(rm->weakMagnitude == 0x00FF);
        REQUIRE(rm->durationMs == 0x00FF);
    }
    SECTION("high byte only") {
        auto p = rumblePayload(0, 0xFF00, 0xFF00, 0xFF00);
        const auto rm = SatelliteClient::parseRumbleMessage(p.data(), p.size());
        REQUIRE(rm.has_value());
        REQUIRE(rm->strongMagnitude == 0xFF00);
        REQUIRE(rm->weakMagnitude == 0xFF00);
        REQUIRE(rm->durationMs == 0xFF00);
    }
    SECTION("value that flips meaning if endianness is wrong") {
        // 0x0100 BE = 256 (correct); LE = 0x0001 (wrong).
        auto p = rumblePayload(0, 0x0100, 0x0100, 0x0100);
        const auto rm = SatelliteClient::parseRumbleMessage(p.data(), p.size());
        REQUIRE(rm.has_value());
        REQUIRE(rm->strongMagnitude == 0x0100);
        REQUIRE(rm->weakMagnitude == 0x0100);
        REQUIRE(rm->durationMs == 0x0100);
    }
}

TEST_CASE("parseRumbleMessage accepts the minimum exact-length payload", "[rumble]") {
    // A payload of exactly kRumblePayloadLen bytes is the canonical wire shape.
    auto p = rumblePayload(1, 0x0100, 0x0080, 250);
    REQUIRE(p.size() == SatelliteClient::kRumblePayloadLen);
    const auto rm = SatelliteClient::parseRumbleMessage(p.data(), p.size());
    REQUIRE(rm.has_value());
    REQUIRE(rm->controllerIndex == 1);
    REQUIRE(rm->durationMs == 250);
}
