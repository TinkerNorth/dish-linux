// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

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
    std::array<std::uint8_t, 12> p{};
    p[0] = 7;
    p[1] = 0x11;
    p[2] = 0x22;
    p[3] = 0x33;
    p[8] = 0xFF;
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

// CAP_LIGHTBAR: the capability bit the slot's REST descriptor advertises when
// the bound pad has an addressable RGB LED.

TEST_CASE("CAP_LIGHTBAR constant pins the wire value", "[lightbar][caps]") {
    REQUIRE(SatelliteClient::kCapLightbar == 0x0008);
    REQUIRE((SatelliteClient::kCapLightbar & SatelliteClient::kCapAnalogTriggers) == 0);
    REQUIRE((SatelliteClient::kCapLightbar & SatelliteClient::kCapRumble) == 0);
    REQUIRE((SatelliteClient::kCapLightbar & SatelliteClient::kCapMotion) == 0);
}

TEST_CASE("withLightbarCapability sets bit 0x0008 iff the pad has an LED", "[lightbar][caps]") {
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
    for (std::uint16_t base : {std::uint16_t{0x0000}, std::uint16_t{0x0007}, std::uint16_t{0x0001},
                               std::uint16_t{0xFFF7}}) {
        const std::uint16_t with = SatelliteClient::withLightbarCapability(base, true);
        const std::uint16_t without = SatelliteClient::withLightbarCapability(base, false);
        REQUIRE(without == base);
        REQUIRE((with & ~static_cast<std::uint16_t>(0x0008)) == base);
        REQUIRE((with | static_cast<std::uint16_t>(0x0008)) == with);
    }
}

TEST_CASE("withLightbarCapability is idempotent when the bit is already set", "[lightbar][caps]") {
    const std::uint16_t base = 0x0007 | SatelliteClient::kCapLightbar;
    REQUIRE(SatelliteClient::withLightbarCapability(base, true) == base);
    REQUIRE(SatelliteClient::withLightbarCapability(base, false) == base);
}
