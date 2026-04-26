// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "Network/LANDiscovery.h"

#include <catch2/catch_test_macros.hpp>

using dish::net::LANDiscovery;

TEST_CASE("parseBeacon accepts a valid satellite beacon", "[discovery]") {
    const QString payload = QStringLiteral(
        R"({"service":"satellite","name":"office","udpPort":9876,"pairPort":9878,"httpPort":9877})");
    const auto s = LANDiscovery::parseBeacon(payload, "10.0.0.1");
    REQUIRE(s.has_value());
    REQUIRE(s->name == "office");
    REQUIRE(s->ip == "10.0.0.1");
    REQUIRE(s->udpPort == 9876);
    REQUIRE(s->pairPort == 9878);
    REQUIRE(s->httpPort == 9877);
}

TEST_CASE("parseBeacon rejects payloads from other services", "[discovery]") {
    const QString payload = QStringLiteral(R"({"service":"chromecast","name":"foo"})");
    REQUIRE_FALSE(LANDiscovery::parseBeacon(payload, "10.0.0.1").has_value());
}

TEST_CASE("parseBeacon rejects malformed JSON", "[discovery]") {
    REQUIRE_FALSE(LANDiscovery::parseBeacon("not json", "10.0.0.1").has_value());
    REQUIRE_FALSE(
        LANDiscovery::parseBeacon(QStringLiteral(R"({"service":"satellite",)"), "10.0.0.1")
            .has_value());
}

TEST_CASE("parseBeacon rejects beacons with an empty name", "[discovery]") {
    const QString payload = QStringLiteral(R"({"service":"satellite","name":""})");
    REQUIRE_FALSE(LANDiscovery::parseBeacon(payload, "10.0.0.1").has_value());
}

TEST_CASE("parseBeacon overrides any beacon-supplied ip with the observed source", "[discovery]") {
    const QString payload =
        QStringLiteral(R"({"service":"satellite","name":"office","ip":"1.1.1.1"})");
    const auto s = LANDiscovery::parseBeacon(payload, "10.0.0.7");
    REQUIRE(s.has_value());
    REQUIRE(s->ip == "10.0.0.7");
}
