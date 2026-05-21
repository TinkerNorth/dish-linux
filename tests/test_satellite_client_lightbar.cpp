// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

// Coverage for SatelliteClient::parseLightbarMessage — the pure decoder for
// the satellite → dish MSG_LIGHTBAR payload (Task 1.4 dedicated stream).
// Same pattern as test_satellite_client_rumble.cpp.

#include "Network/SatelliteClient.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>

using dish::net::SatelliteClient;

TEST_CASE("parseLightbarMessage decodes the 4-byte payload", "[lightbar]") {
    const std::array<std::uint8_t, 4> p = {/*ctrlIdx=*/3, /*r=*/0xDE, /*g=*/0xAD, /*b=*/0xBE};
    const auto lm = SatelliteClient::parseLightbarMessage(p.data(), p.size());
    REQUIRE(lm.has_value());
    REQUIRE(lm->controllerIndex == 3);
    REQUIRE(lm->r == 0xDE);
    REQUIRE(lm->g == 0xAD);
    REQUIRE(lm->b == 0xBE);
}

TEST_CASE("parseLightbarMessage rejects short payloads", "[lightbar]") {
    std::array<std::uint8_t, 3> shortPayload{};
    REQUIRE_FALSE(SatelliteClient::parseLightbarMessage(shortPayload.data(), shortPayload.size())
                      .has_value());

    REQUIRE_FALSE(SatelliteClient::parseLightbarMessage(nullptr, 0).has_value());
    REQUIRE_FALSE(SatelliteClient::parseLightbarMessage(shortPayload.data(), 0).has_value());
}

TEST_CASE("parseLightbarMessage tolerates extra trailing bytes (forward-compat)", "[lightbar]") {
    // Future protocol extensions may append fields. The decoder should accept
    // the leading 4 bytes and ignore the rest.
    std::array<std::uint8_t, 12> p{};
    p[0] = 7;
    p[1] = 0x11;
    p[2] = 0x22;
    p[3] = 0x33;
    p[8] = 0xFF; // pretend-future field
    const auto lm = SatelliteClient::parseLightbarMessage(p.data(), p.size());
    REQUIRE(lm.has_value());
    REQUIRE(lm->controllerIndex == 7);
    REQUIRE(lm->r == 0x11);
    REQUIRE(lm->g == 0x22);
    REQUIRE(lm->b == 0x33);
}

TEST_CASE("parseLightbarMessage handles zero / max RGB values", "[lightbar]") {
    SECTION("all zero") {
        const std::array<std::uint8_t, 4> p = {0, 0, 0, 0};
        const auto lm = SatelliteClient::parseLightbarMessage(p.data(), p.size());
        REQUIRE(lm.has_value());
        REQUIRE(lm->r == 0);
        REQUIRE(lm->g == 0);
        REQUIRE(lm->b == 0);
    }
    SECTION("all 0xFF") {
        const std::array<std::uint8_t, 4> p = {0xFF, 0xFF, 0xFF, 0xFF};
        const auto lm = SatelliteClient::parseLightbarMessage(p.data(), p.size());
        REQUIRE(lm.has_value());
        REQUIRE(lm->controllerIndex == 0xFF);
        REQUIRE(lm->r == 0xFF);
        REQUIRE(lm->g == 0xFF);
        REQUIRE(lm->b == 0xFF);
    }
}

TEST_CASE("MSG_LIGHTBAR constant pins the wire value", "[lightbar][constants]") {
    REQUIRE(SatelliteClient::kMsgLightbar == 0x000D);
}

// ---------------------------------------------------------------------------
// CAP_LIGHTBAR (Task 1.4) — the per-controller capability bit advertised in
// MSG_CONTROLLER_ADD when the bound pad has an addressable RGB LED.
// ---------------------------------------------------------------------------

TEST_CASE("CAP_LIGHTBAR constant pins the wire value", "[lightbar][caps]") {
    REQUIRE(SatelliteClient::kCapLightbar == 0x0008);
    // It must not collide with any existing capability bit.
    REQUIRE((SatelliteClient::kCapLightbar & SatelliteClient::kCapAnalogTriggers) == 0);
    REQUIRE((SatelliteClient::kCapLightbar & SatelliteClient::kCapRumble) == 0);
    REQUIRE((SatelliteClient::kCapLightbar & SatelliteClient::kCapMotion) == 0);
}

TEST_CASE("withLightbarCapability sets bit 0x0008 iff the pad has an LED", "[lightbar][caps]") {
    // The static base word dish advertises today: analog triggers | rumble |
    // motion == 0x0007.
    const std::uint16_t base = SatelliteClient::kCapAnalogTriggers | SatelliteClient::kCapRumble |
                               SatelliteClient::kCapMotion;
    REQUIRE(base == 0x0007);

    SECTION("controller has an LED -> CAP_LIGHTBAR is OR-ed in") {
        const std::uint16_t caps = SatelliteClient::withLightbarCapability(base, true);
        REQUIRE(caps == 0x000F);
        REQUIRE((caps & SatelliteClient::kCapLightbar) != 0);
    }
    SECTION("controller has no LED -> word is unchanged") {
        const std::uint16_t caps = SatelliteClient::withLightbarCapability(base, false);
        REQUIRE(caps == 0x0007);
        REQUIRE((caps & SatelliteClient::kCapLightbar) == 0);
    }
}

TEST_CASE("withLightbarCapability leaves the other capability bits intact", "[lightbar][caps]") {
    // Whatever the base word is, CAP_LIGHTBAR must only ever add bit 0x0008 —
    // never clear or change another bit.
    for (std::uint16_t base : {std::uint16_t{0x0000}, std::uint16_t{0x0007}, std::uint16_t{0x0001},
                               std::uint16_t{0xFFF7}}) {
        const std::uint16_t with = SatelliteClient::withLightbarCapability(base, true);
        const std::uint16_t without = SatelliteClient::withLightbarCapability(base, false);
        // "without" is a pure identity.
        REQUIRE(without == base);
        // "with" differs from the base only in bit 0x0008.
        REQUIRE((with & ~static_cast<std::uint16_t>(0x0008)) == base);
        REQUIRE((with | static_cast<std::uint16_t>(0x0008)) == with);
    }
}

TEST_CASE("withLightbarCapability is idempotent when the bit is already set", "[lightbar][caps]") {
    // A base that already carries CAP_LIGHTBAR stays a single 0x0008 bit.
    const std::uint16_t base = 0x0007 | SatelliteClient::kCapLightbar;
    REQUIRE(SatelliteClient::withLightbarCapability(base, true) == base);
    REQUIRE(SatelliteClient::withLightbarCapability(base, false) == base);
}
