// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The manager's async connect FSM can't be driven without sockets, so these pin
// the relearn DATA contract at the seam it calls instead: a discovery scan
// re-points a remembered satellite's endpoint while its id and key survive.

#include "Network/ConnectionStore.h"

#include <catch2/catch_test_macros.hpp>

#include <QDir>
#include <QSettings>
#include <QString>
#include <QUuid>

#include <memory>

using dish::models::DiscoveredServer;
using dish::models::RememberedWifi;

namespace {

DiscoveredServer server(const QString& machineId, const QString& ip,
                        const QString& name = QStringLiteral("Pc"), int udpPort = 9876,
                        int pairPort = 9443, int httpPort = 9443) {
    DiscoveredServer s;
    s.machineId = machineId;
    s.ip = ip;
    s.name = name;
    s.udpPort = udpPort;
    s.pairPort = pairPort;
    s.httpPort = httpPort;
    return s;
}

QString midId(const QString& machineId) { return QStringLiteral("mid:") + machineId; }

// Backed by an isolated temp INI so no test ever touches the real registry.
dish::net::ConnectionStore makeStore() {
    const QString path = QDir::tempPath() + QStringLiteral("/dish-reconnect-") +
                         QUuid::createUuid().toString(QUuid::WithoutBraces) +
                         QStringLiteral(".ini");
    return dish::net::ConnectionStore(std::make_unique<QSettings>(path, QSettings::IniFormat));
}

const QString kKeyAA = QString(64, QLatin1Char('a'));

RememberedWifi rowFor(const dish::net::ConnectionStore& store, const QString& id) {
    for (const auto& r : store.remembered()) {
        if (r.id == id) { return r; }
    }
    return {};
}

} // namespace

TEST_CASE("relearn re-points a remembered satellite's endpoint in place (adapter seam)",
          "[reconnect]") {
    auto store = makeStore();
    store.remember(server("m1", "10.0.0.5"));
    REQUIRE(store.remembered().size() == 1);
    CHECK(rowFor(store, midId("m1")).ip == QStringLiteral("10.0.0.5"));

    // The same box comes back on a new DHCP lease.
    store.refreshFromDiscovery({server("m1", "10.0.0.99")});

    REQUIRE(store.remembered().size() == 1);
    CHECK(rowFor(store, midId("m1")).ip == QStringLiteral("10.0.0.99"));
}

TEST_CASE("after a relearn, toDiscovered yields the fresh endpoint autoReconnect would dial",
          "[reconnect]") {
    auto store = makeStore();
    store.remember(server("m1", "10.0.0.5", QStringLiteral("Den"), 9876, 9443, 9443));
    store.refreshFromDiscovery(
        {server("m1", "10.0.0.99", QStringLiteral("Den"), 9876, 9444, 9445)});

    // autoReconnectAll / scheduleRetry both dial remembered() -> toDiscovered(),
    // so that server has to carry the current address and ports.
    const DiscoveredServer dialed = rowFor(store, midId("m1")).toDiscovered();
    CHECK(dialed.ip == QStringLiteral("10.0.0.99"));
    CHECK(dialed.pairPort == 9444);
    CHECK(dialed.httpPort == 9445);
    CHECK(dialed.machineId == QStringLiteral("m1"));
    CHECK(dialed.isValid());
    // The stable id survives, so the connection and its key keep matching.
    CHECK(dialed.id() == midId("m1"));
}

TEST_CASE("a relearn never adds an un-remembered satellite", "[reconnect]") {
    auto store = makeStore();
    store.remember(server("m1", "10.0.0.5"));
    store.refreshFromDiscovery({server("m1", "10.0.0.99"), server("m2", "10.0.0.6")});

    const auto rows = store.remembered();
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].id == midId("m1"));
    CHECK(rows[0].ip == QStringLiteral("10.0.0.99"));
}

TEST_CASE("the pairing key survives a relearn (no PIN needed on reconnect)", "[reconnect]") {
    auto store = makeStore();
    store.remember(server("m1", "10.0.0.5"));
    // The key hangs off the stable id (machineId), independent of the IP.
    store.setSharedKey(kKeyAA, midId("m1"));

    store.refreshFromDiscovery({server("m1", "10.0.0.99")});

    // Still resolving under the same id is why credentialsFor() succeeds and
    // connectTo skips the pair handshake.
    CHECK(store.sharedKey(midId("m1")) == kKeyAA);
    CHECK(rowFor(store, midId("m1")).ip == QStringLiteral("10.0.0.99"));
}

TEST_CASE("a beacon without a machineId never re-points a remembered row", "[reconnect]") {
    auto store = makeStore();
    store.remember(server("m1", "10.0.0.5"));
    // Without a machineId the beacon can't be matched to the stable identity.
    store.refreshFromDiscovery({server("", "10.0.0.99")});
    CHECK(rowFor(store, midId("m1")).ip == QStringLiteral("10.0.0.5"));
}
