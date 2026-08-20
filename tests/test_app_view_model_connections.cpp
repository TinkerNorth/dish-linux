// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// AppViewModel cannot be instantiated in this harness (it owns a full AppModel:
// SDL bridge, theme controller over qApp), so these pin the data contract it
// folds rather than the view model itself. Command forwarding is covered against
// the real manager in test_connection_coordinator.cpp.

#include "Models/Models.h"
#include "Network/ConnectionStore.h"
#include "Network/WifiConnectionManager.h"

#include "QSettingsFixture.h"

#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QObject>
#include <QString>

#include <memory>

using dish::models::DiscoveredServer;
using dish::models::DiscoverySource;
using dish::models::discoverySourceLabel;
using dish::test::makeSharedSettings;

namespace {

DiscoveredServer sat(const QString& machineId, const QString& ip, DiscoverySource src) {
    DiscoveredServer s;
    s.machineId = machineId;
    s.ip = ip;
    s.source = src;
    return s;
}

} // namespace

// Discovery reactivity is NOT covered here: driving it means calling the real
// manager's startDiscovery(), which launches a blocking Winsock/mDNS scan on the
// global thread pool that the test process must join on exit, and there is no
// network seam to substitute. It hangs the run, so it is verified at runtime.

TEST_CASE("discoverySourceLabel maps every source to the FOUND-row label",
          "[appvm][connections][source]") {
    CHECK(discoverySourceLabel(DiscoverySource::Broadcast) == QStringLiteral("UDP broadcast"));
    CHECK(discoverySourceLabel(DiscoverySource::Mdns) == QStringLiteral("mDNS"));
    CHECK(discoverySourceLabel(DiscoverySource::Both) == QStringLiteral("mDNS + broadcast"));
}

TEST_CASE("a discovered server keeps its source for the FOUND-row label",
          "[appvm][connections][source]") {
    const auto s = sat(QStringLiteral("m1"), QStringLiteral("10.0.0.1"), DiscoverySource::Both);
    CHECK(discoverySourceLabel(s.source) == QStringLiteral("mDNS + broadcast"));
}

TEST_CASE("connectByServerId/pairByServerId resolve on the stable machineId-preferring id",
          "[appvm][connections][deraced]") {
    // The id-based invokables key on DiscoveredServer::id(), not a list index, so
    // a reorder of discoveredServers() between read and call cannot connect the
    // wrong box.
    const auto withMid =
        sat(QStringLiteral("m1"), QStringLiteral("10.0.0.9"), DiscoverySource::Broadcast);
    CHECK(withMid.id() == QStringLiteral("mid:m1")); // machineId-preferring

    // Same machineId, new IP: the id is address-independent, so a DHCP move does
    // not break the resolution.
    auto moved = withMid;
    moved.ip = QStringLiteral("10.0.0.250");
    CHECK(moved.id() == withMid.id());

    // No machineId: the id falls back to the ip:udpPort pair.
    DiscoveredServer legacy;
    legacy.ip = QStringLiteral("10.0.0.5");
    legacy.udpPort = 9876;
    CHECK(legacy.id() == QStringLiteral("wifi:10.0.0.5:9876"));
}
