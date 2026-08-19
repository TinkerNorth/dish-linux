// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Port precedence is TXT > SRV (when > 0) > compiled-in defaults.

#include "source/connection/MdnsDiscovery.h"

#include <catch2/catch_test_macros.hpp>

#include <QByteArray>
#include <QHash>
#include <QString>

using dish::models::DiscoverySource;
using dish::net::kMdnsDefaultHttp;
using dish::net::kMdnsDefaultPair;
using dish::net::kMdnsDefaultUdp;
using dish::net::mdnsServiceToServer;
using dish::net::mdnsTxtInt;
using dish::net::mdnsTxtString;

namespace {
QByteArray b(const char* s) { return QByteArray(s); }
} // namespace

TEST_CASE("mdnsTxtInt parses a numeric value", "[mdnsmap]") {
    QHash<QString, QByteArray> txt{{"udp", b("9876")}};
    CHECK(mdnsTxtInt(txt, "udp") == 9876);
}

TEST_CASE("mdnsTxtInt trims surrounding whitespace", "[mdnsmap]") {
    QHash<QString, QByteArray> txt{{"pair", b(" 9878 ")}};
    CHECK(mdnsTxtInt(txt, "pair") == 9878);
}

TEST_CASE("mdnsTxtInt is null for a missing key", "[mdnsmap]") {
    QHash<QString, QByteArray> txt{{"udp", b("9876")}};
    CHECK_FALSE(mdnsTxtInt(txt, "http").has_value());
}

TEST_CASE("mdnsTxtInt is null for a null value", "[mdnsmap]") {
    QHash<QString, QByteArray> txt{{"udp", QByteArray()}};
    CHECK_FALSE(mdnsTxtInt(txt, "udp").has_value());
}

TEST_CASE("mdnsTxtInt is null for a non-numeric value", "[mdnsmap]") {
    QHash<QString, QByteArray> txt{{"udp", b("not-a-port")}};
    CHECK_FALSE(mdnsTxtInt(txt, "udp").has_value());
}

TEST_CASE("mdnsServiceToServer returns nullopt for a null host", "[mdnsmap]") {
    CHECK_FALSE(mdnsServiceToServer("sat", "", 9876, {}).has_value());
}

TEST_CASE("mdnsServiceToServer tags the resolved server MDNS", "[mdnsmap]") {
    const auto s = mdnsServiceToServer("sat", "10.0.0.7", 9876, {});
    REQUIRE(s.has_value());
    CHECK(s->source == DiscoverySource::Mdns);
}

TEST_CASE("mdnsServiceToServer falls back to ip for an empty service name", "[mdnsmap]") {
    const auto s = mdnsServiceToServer("", "10.0.0.7", 9876, {});
    REQUIRE(s.has_value());
    CHECK(s->name == QStringLiteral("10.0.0.7"));
}

TEST_CASE("mdnsServiceToServer keeps a non-empty service name", "[mdnsmap]") {
    const auto s = mdnsServiceToServer("Living Room PC", "10.0.0.7", 9876, {});
    REQUIRE(s.has_value());
    CHECK(s->name == QStringLiteral("Living Room PC"));
}

TEST_CASE("mdnsServiceToServer prefers the TXT udp port over SRV", "[mdnsmap]") {
    QHash<QString, QByteArray> txt{{"udp", b("9900")}};
    const auto s = mdnsServiceToServer("sat", "10.0.0.7", 9876, txt);
    REQUIRE(s.has_value());
    CHECK(s->udpPort == 9900);
}

TEST_CASE("mdnsServiceToServer falls back to the SRV port without a TXT udp", "[mdnsmap]") {
    const auto s = mdnsServiceToServer("sat", "10.0.0.7", 9881, {});
    REQUIRE(s.has_value());
    CHECK(s->udpPort == 9881);
}

TEST_CASE("mdnsServiceToServer uses the default udp port when SRV is zero", "[mdnsmap]") {
    const auto s = mdnsServiceToServer("sat", "10.0.0.7", 0, {});
    REQUIRE(s.has_value());
    CHECK(s->udpPort == kMdnsDefaultUdp);
}

TEST_CASE("mdnsServiceToServer uses the default udp port when SRV is negative", "[mdnsmap]") {
    const auto s = mdnsServiceToServer("sat", "10.0.0.7", -1, {});
    REQUIRE(s.has_value());
    CHECK(s->udpPort == kMdnsDefaultUdp);
}

TEST_CASE("mdnsServiceToServer takes pair and http from TXT", "[mdnsmap]") {
    QHash<QString, QByteArray> txt{{"pair", b("19878")}, {"http", b("18080")}};
    const auto s = mdnsServiceToServer("sat", "10.0.0.7", 9876, txt);
    REQUIRE(s.has_value());
    CHECK(s->pairPort == 19878);
    CHECK(s->httpPort == 18080);
}

TEST_CASE("mdnsServiceToServer falls pair and http back to defaults", "[mdnsmap]") {
    const auto s = mdnsServiceToServer("sat", "10.0.0.7", 9876, {});
    REQUIRE(s.has_value());
    CHECK(s->pairPort == kMdnsDefaultPair);
    CHECK(s->httpPort == kMdnsDefaultHttp);
}

TEST_CASE("mdnsServiceToServer resolves all three ports independently", "[mdnsmap]") {
    QHash<QString, QByteArray> txt{{"udp", b("9001")}, {"pair", b("9002")}, {"http", b("9003")}};
    const auto s = mdnsServiceToServer("Full", "192.168.1.50", 1234, txt);
    REQUIRE(s.has_value());
    CHECK(s->name == QStringLiteral("Full"));
    CHECK(s->ip == QStringLiteral("192.168.1.50"));
    CHECK(s->udpPort == 9001);
    CHECK(s->pairPort == 9002);
    CHECK(s->httpPort == 9003);
}

TEST_CASE("mdnsServiceToServer falls a garbage TXT udp through to SRV", "[mdnsmap]") {
    QHash<QString, QByteArray> txt{{"udp", b("xxxx")}};
    const auto s = mdnsServiceToServer("sat", "10.0.0.7", 9882, txt);
    REQUIRE(s.has_value());
    CHECK(s->udpPort == 9882);
}

TEST_CASE("mdnsServiceToServer reads the machineId from the mid TXT key", "[mdnsmap]") {
    QHash<QString, QByteArray> txt{{"mid", b("deadbeef")}};
    const auto s = mdnsServiceToServer("sat", "10.0.0.7", 9876, txt);
    REQUIRE(s.has_value());
    CHECK(s->machineId == QStringLiteral("deadbeef"));
}

TEST_CASE("mdnsServiceToServer leaves the machineId empty without a mid TXT", "[mdnsmap]") {
    const auto s = mdnsServiceToServer("sat", "10.0.0.7", 9876, {});
    REQUIRE(s.has_value());
    CHECK(s->machineId.isEmpty());
}

TEST_CASE("mdnsTxtString trims and rejects empty or missing values", "[mdnsmap]") {
    QHash<QString, QByteArray> present{{"mid", b(" x ")}};
    CHECK(mdnsTxtString(present, "mid") == QStringLiteral("x"));
    QHash<QString, QByteArray> blank{{"mid", b("   ")}};
    CHECK_FALSE(mdnsTxtString(blank, "mid").has_value());
    QHash<QString, QByteArray> empty;
    CHECK_FALSE(mdnsTxtString(empty, "mid").has_value());
}
