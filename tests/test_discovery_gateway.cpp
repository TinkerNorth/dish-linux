// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The merge de-dupes by stable key: machineId, else ip:udpPort.

#include "Models/Models.h"
#include "source/connection/DiscoveryGateway.h"

#include <catch2/catch_test_macros.hpp>

#include <QString>

using dish::models::DiscoveredServer;
using dish::models::DiscoverySource;
using dish::models::discoverySourceLabel;
using dish::net::DiscoveryGateway;

namespace {
DiscoveredServer server(const QString& name, const QString& ip, int udp = 9876) {
    DiscoveredServer s;
    s.name = name;
    s.ip = ip;
    s.udpPort = udp;
    return s;
}
} // namespace

TEST_CASE("mergeDiscovered tags a broadcast-only server", "[discgw]") {
    const auto m = DiscoveryGateway::mergeDiscovered({server("A", "10.0.0.1")}, {});
    REQUIRE(m.size() == 1);
    CHECK(m[0].source == DiscoverySource::Broadcast);
}

TEST_CASE("mergeDiscovered tags an mDNS-only server", "[discgw]") {
    const auto m = DiscoveryGateway::mergeDiscovered({}, {server("B", "10.0.0.2")});
    REQUIRE(m.size() == 1);
    CHECK(m[0].source == DiscoverySource::Mdns);
}

TEST_CASE("mergeDiscovered tags a server heard on both paths as Both", "[discgw]") {
    const auto m =
        DiscoveryGateway::mergeDiscovered({server("Sat", "10.0.0.9")}, {server("Sat", "10.0.0.9")});
    REQUIRE(m.size() == 1);
    CHECK(m[0].source == DiscoverySource::Both);
}

TEST_CASE("mergeDiscovered keeps distinct servers from each path", "[discgw]") {
    const auto m = DiscoveryGateway::mergeDiscovered({server("Alpha", "10.0.0.1")},
                                                     {server("Bravo", "10.0.0.2")});
    REQUIRE(m.size() == 2);
    CHECK(m[0].name == QStringLiteral("Alpha"));
    CHECK(m[0].source == DiscoverySource::Broadcast);
    CHECK(m[1].name == QStringLiteral("Bravo"));
    CHECK(m[1].source == DiscoverySource::Mdns);
}

TEST_CASE("mergeDiscovered treats same ip + different udpPort as distinct", "[discgw]") {
    const auto m = DiscoveryGateway::mergeDiscovered({server("One", "10.0.0.1", 9876)},
                                                     {server("Two", "10.0.0.1", 9900)});
    CHECK(m.size() == 2);
}

TEST_CASE("mergeDiscovered sorts by name", "[discgw]") {
    const auto m = DiscoveryGateway::mergeDiscovered(
        {server("Zulu", "10.0.0.3"), server("Alpha", "10.0.0.1")}, {server("Mike", "10.0.0.2")});
    REQUIRE(m.size() == 3);
    CHECK(m[0].name == QStringLiteral("Alpha"));
    CHECK(m[1].name == QStringLiteral("Mike"));
    CHECK(m[2].name == QStringLiteral("Zulu"));
}

TEST_CASE("mergeDiscovered yields an empty list for empty inputs", "[discgw]") {
    CHECK(DiscoveryGateway::mergeDiscovered({}, {}).isEmpty());
}

TEST_CASE("DiscoverySource labels are stable and distinct", "[discgw]") {
    CHECK(discoverySourceLabel(DiscoverySource::Broadcast) == QStringLiteral("UDP broadcast"));
    CHECK(discoverySourceLabel(DiscoverySource::Mdns) == QStringLiteral("mDNS"));
    CHECK(discoverySourceLabel(DiscoverySource::Both) == QStringLiteral("mDNS + broadcast"));
}

TEST_CASE("pinId falls back to ip for an empty satellite id", "[discgw]") {
    CHECK(DiscoveryGateway::pinId(QString(), QStringLiteral("10.0.0.7")) ==
          QStringLiteral("10.0.0.7"));
}

TEST_CASE("pinId uses the explicit satellite id when present", "[discgw]") {
    CHECK(
        DiscoveryGateway::pinId(QStringLiteral("satellite:mid:abc"), QStringLiteral("10.0.0.7")) ==
        QStringLiteral("satellite:mid:abc"));
}

TEST_CASE("mergeDiscovered collapses one machineId heard on both paths", "[discgw]") {
    DiscoveredServer b = server("Box", "10.0.0.5", 9876);
    b.machineId = QStringLiteral("m1");
    DiscoveredServer m = server("Box", "10.0.0.99", 9900); // different address, same machineId
    m.machineId = QStringLiteral("m1");
    const auto merged = DiscoveryGateway::mergeDiscovered({b}, {m});
    REQUIRE(merged.size() == 1);
    CHECK(merged[0].source == DiscoverySource::Both);
}
