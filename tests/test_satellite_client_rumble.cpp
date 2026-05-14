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

// Build the mandatory 8-byte rumble payload (no lightbar). Matches the
// producer side in satellite/src/adapters/client_adapter.cpp::sendRumble.
std::array<std::uint8_t, 8> mandatoryPayload(std::uint8_t ctrlIdx, std::uint16_t strong,
                                             std::uint16_t weak, std::uint16_t dur) {
    return {
        ctrlIdx,
        static_cast<std::uint8_t>(strong >> 8),
        static_cast<std::uint8_t>(strong & 0xFF),
        static_cast<std::uint8_t>(weak >> 8),
        static_cast<std::uint8_t>(weak & 0xFF),
        static_cast<std::uint8_t>(dur >> 8),
        static_cast<std::uint8_t>(dur & 0xFF),
        0x00, // flags = 0 ⇒ no lightbar
    };
}

// 11-byte payload with the lightbar flag set + RGB tail.
std::array<std::uint8_t, 11> lightbarPayload(std::uint8_t ctrlIdx, std::uint16_t strong,
                                             std::uint16_t weak, std::uint16_t dur,
                                             std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    return {
        ctrlIdx,
        static_cast<std::uint8_t>(strong >> 8),
        static_cast<std::uint8_t>(strong & 0xFF),
        static_cast<std::uint8_t>(weak >> 8),
        static_cast<std::uint8_t>(weak & 0xFF),
        static_cast<std::uint8_t>(dur >> 8),
        static_cast<std::uint8_t>(dur & 0xFF),
        0x01, // flags bit 0 = lightbar present
        r, g, b,
    };
}

} // namespace

TEST_CASE("parseRumbleMessage decodes mandatory fields", "[rumble]") {
    auto p = mandatoryPayload(/*ctrlIdx=*/3, /*strong=*/0xABCD, /*weak=*/0x1234, /*dur=*/500);
    const auto rm = SatelliteClient::parseRumbleMessage(p.data(), p.size());
    REQUIRE(rm.has_value());
    REQUIRE(rm->controllerIndex == 3);
    REQUIRE(rm->strongMagnitude == 0xABCD);
    REQUIRE(rm->weakMagnitude == 0x1234);
    REQUIRE(rm->durationMs == 500);
    REQUIRE_FALSE(rm->hasLightbar);
    REQUIRE(rm->lightbarR == 0);
    REQUIRE(rm->lightbarG == 0);
    REQUIRE(rm->lightbarB == 0);
}

TEST_CASE("parseRumbleMessage decodes a stop request (all zero magnitudes)", "[rumble]") {
    auto p = mandatoryPayload(/*ctrlIdx=*/0, /*strong=*/0, /*weak=*/0, /*dur=*/0);
    const auto rm = SatelliteClient::parseRumbleMessage(p.data(), p.size());
    REQUIRE(rm.has_value());
    REQUIRE(rm->strongMagnitude == 0);
    REQUIRE(rm->weakMagnitude == 0);
    REQUIRE(rm->durationMs == 0);
}

TEST_CASE("parseRumbleMessage decodes max-magnitude payload without overflow", "[rumble]") {
    auto p = mandatoryPayload(/*ctrlIdx=*/0xFF, /*strong=*/0xFFFF, /*weak=*/0xFFFF, /*dur=*/0xFFFF);
    const auto rm = SatelliteClient::parseRumbleMessage(p.data(), p.size());
    REQUIRE(rm.has_value());
    REQUIRE(rm->controllerIndex == 0xFF);
    REQUIRE(rm->strongMagnitude == 0xFFFF);
    REQUIRE(rm->weakMagnitude == 0xFFFF);
    REQUIRE(rm->durationMs == 0xFFFF);
}

TEST_CASE("parseRumbleMessage decodes lightbar tail", "[rumble]") {
    auto p = lightbarPayload(/*ctrlIdx=*/1, /*strong=*/0x0100, /*weak=*/0x0080, /*dur=*/250,
                             /*r=*/0xDE, /*g=*/0xAD, /*b=*/0xBE);
    const auto rm = SatelliteClient::parseRumbleMessage(p.data(), p.size());
    REQUIRE(rm.has_value());
    REQUIRE(rm->hasLightbar);
    REQUIRE(rm->lightbarR == 0xDE);
    REQUIRE(rm->lightbarG == 0xAD);
    REQUIRE(rm->lightbarB == 0xBE);
}

TEST_CASE("parseRumbleMessage rejects truncated mandatory section", "[rumble]") {
    // Anything shorter than 8 bytes is malformed — the satellite never emits
    // such a packet, but a malicious / racing peer could.
    std::array<std::uint8_t, 7> shortPayload{};
    REQUIRE_FALSE(SatelliteClient::parseRumbleMessage(shortPayload.data(), shortPayload.size()).has_value());

    REQUIRE_FALSE(SatelliteClient::parseRumbleMessage(nullptr, 0).has_value());
    REQUIRE_FALSE(SatelliteClient::parseRumbleMessage(shortPayload.data(), 0).has_value());
}

TEST_CASE("parseRumbleMessage rejects lightbar flag with truncated tail", "[rumble]") {
    // Flags say "lightbar present" but the payload doesn't carry the 3 RGB
    // bytes. Treat as malformed rather than guessing zeros — that would mask
    // wire-protocol bugs on the satellite side.
    auto p = mandatoryPayload(0, 0, 0, 0);
    p[7] = 0x01; // flag set, but only 8 bytes total
    REQUIRE_FALSE(SatelliteClient::parseRumbleMessage(p.data(), p.size()).has_value());

    // 10 bytes is also short — RGB needs 3.
    std::array<std::uint8_t, 10> p10{};
    p10[7] = 0x01;
    REQUIRE_FALSE(SatelliteClient::parseRumbleMessage(p10.data(), p10.size()).has_value());
}

TEST_CASE("parseRumbleMessage tolerates extra trailing bytes (forward-compat)", "[rumble]") {
    // Future protocol extensions may append fields after the lightbar tail.
    // The decoder must return successfully and ignore the unknown bytes.
    std::vector<std::uint8_t> p(20, 0xAA);
    auto base = lightbarPayload(2, 100, 50, 700, 1, 2, 3);
    std::copy(base.begin(), base.end(), p.begin());
    const auto rm = SatelliteClient::parseRumbleMessage(p.data(), p.size());
    REQUIRE(rm.has_value());
    REQUIRE(rm->controllerIndex == 2);
    REQUIRE(rm->strongMagnitude == 100);
    REQUIRE(rm->lightbarR == 1);
    REQUIRE(rm->lightbarG == 2);
    REQUIRE(rm->lightbarB == 3);
}

TEST_CASE("parseRumbleMessage flags bit 1+ are reserved (treated as not-lightbar)", "[rumble]") {
    // Only bit 0 currently means anything. Higher bits should NOT toggle
    // lightbar parsing — that's reserved for future use. Test that the
    // decoder strictly checks bit 0.
    auto p = mandatoryPayload(0, 0, 0, 0);
    p[7] = 0x02; // bit 1 set, bit 0 clear
    const auto rm = SatelliteClient::parseRumbleMessage(p.data(), p.size());
    REQUIRE(rm.has_value());
    REQUIRE_FALSE(rm->hasLightbar);
}

TEST_CASE("parseRumbleMessage handles big-endian boundary cases", "[rumble]") {
    // Cover the byte-swap path: 0x00FF (low byte set), 0xFF00 (high byte set),
    // and 0x0100 (a value where naive little-endian read would be wrong).
    SECTION("low byte only") {
        auto p = mandatoryPayload(0, 0x00FF, 0x00FF, 0x00FF);
        const auto rm = SatelliteClient::parseRumbleMessage(p.data(), p.size());
        REQUIRE(rm.has_value());
        REQUIRE(rm->strongMagnitude == 0x00FF);
        REQUIRE(rm->weakMagnitude == 0x00FF);
        REQUIRE(rm->durationMs == 0x00FF);
    }
    SECTION("high byte only") {
        auto p = mandatoryPayload(0, 0xFF00, 0xFF00, 0xFF00);
        const auto rm = SatelliteClient::parseRumbleMessage(p.data(), p.size());
        REQUIRE(rm.has_value());
        REQUIRE(rm->strongMagnitude == 0xFF00);
        REQUIRE(rm->weakMagnitude == 0xFF00);
        REQUIRE(rm->durationMs == 0xFF00);
    }
    SECTION("value that flips meaning if endianness is wrong") {
        // 0x0100 BE = 256 (correct); LE = 0x0001 (wrong).
        auto p = mandatoryPayload(0, 0x0100, 0x0100, 0x0100);
        const auto rm = SatelliteClient::parseRumbleMessage(p.data(), p.size());
        REQUIRE(rm.has_value());
        REQUIRE(rm->strongMagnitude == 0x0100);
        REQUIRE(rm->weakMagnitude == 0x0100);
        REQUIRE(rm->durationMs == 0x0100);
    }
}
