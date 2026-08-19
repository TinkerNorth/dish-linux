// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// A satellite is keyed on DiscoveredServer::id(): "mid:<machineId>" when the
// beacon carries one, else the legacy "wifi:<ip>:<udpPort>".

#include "repository/ConnectionStore.h"

#include "QSettingsFixture.h"

#include <catch2/catch_test_macros.hpp>

#include <QString>

using dish::models::DiscoveredServer;
using dish::repository::ConnectionStore;
using dish::test::makeSharedSettings;

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
QString legacyId(const QString& ip, int udpPort = 9876) {
    return QStringLiteral("wifi:%1:%2").arg(ip).arg(udpPort);
}

const QString kKeyAA = QString(64, QLatin1Char('a'));
const QString kKeyBB = QString(64, QLatin1Char('b'));

} // namespace

TEST_CASE("same machine at a new IP stays one row under the stable id", "[cstore]") {
    ConnectionStore store(makeSharedSettings());
    store.rememberSatellite(server("m1", "10.0.0.5"));
    store.rememberSatellite(server("m1", "10.0.0.99"));
    const auto rows = store.remembered();
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].id == midId("m1"));
    CHECK(rows[0].ip == QStringLiteral("10.0.0.99"));
}

TEST_CASE("two different machines stay two rows", "[cstore]") {
    ConnectionStore store(makeSharedSettings());
    store.rememberSatellite(server("m1", "10.0.0.5"));
    store.rememberSatellite(server("m2", "10.0.0.5"));
    CHECK(store.remembered().size() == 2);
}

TEST_CASE("the pairing key is keyed on the stable id and dies with forget", "[cstore]") {
    ConnectionStore store(makeSharedSettings());
    store.rememberSatellite(server("m1", "10.0.0.5"));
    store.setSatelliteSharedKey(midId("m1"), kKeyAA);
    store.forgetSatellite(midId("m1"));
    CHECK_FALSE(store.satelliteSharedKey(midId("m1")).has_value());
    CHECK(store.remembered().isEmpty());
}

TEST_CASE("an identity upgrade migrates the legacy row's key without a re-pair", "[cstore]") {
    ConnectionStore store(makeSharedSettings());
    store.rememberSatellite(server("", "10.0.0.5")); // legacy row, no machineId
    store.setSatelliteSharedKey(legacyId("10.0.0.5"), kKeyAA);
    store.rememberSatellite(server("m1", "10.0.0.5")); // same box, now with a machineId
    const auto rows = store.remembered();
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].id == midId("m1"));
    CHECK(store.satelliteSharedKey(midId("m1")) == kKeyAA);
    CHECK_FALSE(store.satelliteSharedKey(legacyId("10.0.0.5")).has_value());
}

TEST_CASE("an identity upgrade keeps the stable row's own key and purges the legacy", "[cstore]") {
    ConnectionStore store(makeSharedSettings());
    store.rememberSatellite(server("", "10.0.0.5"));
    store.setSatelliteSharedKey(legacyId("10.0.0.5"), kKeyBB);
    store.setSatelliteSharedKey(midId("m1"), kKeyAA); // stable row already has its own key
    store.rememberSatellite(server("m1", "10.0.0.5"));
    CHECK(store.satelliteSharedKey(midId("m1")) == kKeyAA);
    CHECK_FALSE(store.satelliteSharedKey(legacyId("10.0.0.5")).has_value());
}

TEST_CASE("a beacon without a machineId refreshes a known stable row, no ghost", "[cstore]") {
    ConnectionStore store(makeSharedSettings());
    store.rememberSatellite(server("m1", "10.0.0.5", QStringLiteral("Old")));
    store.rememberSatellite(server("", "10.0.0.5", QStringLiteral("New")));
    const auto rows = store.remembered();
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].id == midId("m1"));
    CHECK(rows[0].name == QStringLiteral("New"));
}

TEST_CASE("a scan re-points a remembered satellite at its current address", "[cstore]") {
    ConnectionStore store(makeSharedSettings());
    store.rememberSatellite(server("m1", "10.0.0.5"));
    store.refreshFromDiscovery({server("m1", "10.0.0.99")});
    const auto rows = store.remembered();
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].id == midId("m1"));
    CHECK(rows[0].ip == QStringLiteral("10.0.0.99"));
}

TEST_CASE("a scan refreshes the name and ports alongside the address", "[cstore]") {
    ConnectionStore store(makeSharedSettings());
    store.rememberSatellite(server("m1", "10.0.0.5", QStringLiteral("Old")));
    store.refreshFromDiscovery(
        {server("m1", "10.0.0.5", QStringLiteral("Renamed"), 9876, 9444, 9445)});
    const auto rows = store.remembered();
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].name == QStringLiteral("Renamed"));
    CHECK(rows[0].pairPort == 9444);
    CHECK(rows[0].httpPort == 9445);
}

TEST_CASE("a scan never adds an un-remembered satellite", "[cstore]") {
    ConnectionStore store(makeSharedSettings());
    store.rememberSatellite(server("m1", "10.0.0.5"));
    store.refreshFromDiscovery({server("m2", "10.0.0.6")});
    const auto rows = store.remembered();
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].id == midId("m1"));
}

TEST_CASE("a beacon without a machineId never re-points a row", "[cstore]") {
    ConnectionStore store(makeSharedSettings());
    store.rememberSatellite(server("m1", "10.0.0.5"));
    store.refreshFromDiscovery({server("", "10.0.0.99")});
    const auto rows = store.remembered();
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].ip == QStringLiteral("10.0.0.5"));
}

TEST_CASE("a scan upgrades a legacy row to a stable id and carries the key", "[cstore]") {
    ConnectionStore store(makeSharedSettings());
    store.rememberSatellite(server("", "10.0.0.5")); // legacy
    store.setSatelliteSharedKey(legacyId("10.0.0.5"), kKeyAA);
    store.refreshFromDiscovery({server("m1", "10.0.0.5")});
    const auto rows = store.remembered();
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].id == midId("m1"));
    CHECK(store.satelliteSharedKey(midId("m1")) == kKeyAA);
    CHECK_FALSE(store.satelliteSharedKey(legacyId("10.0.0.5")).has_value());
}

TEST_CASE("a scan refreshes only the rows it saw", "[cstore]") {
    ConnectionStore store(makeSharedSettings());
    store.rememberSatellite(server("m1", "10.0.0.5"));
    store.rememberSatellite(server("m2", "10.0.0.6"));
    store.refreshFromDiscovery({server("m1", "10.0.0.99")});
    const auto rows = store.remembered();
    REQUIRE(rows.size() == 2);
    QString m1Ip, m2Ip;
    for (const auto& r : rows) {
        if (r.id == midId("m1")) { m1Ip = r.ip; }
        if (r.id == midId("m2")) { m2Ip = r.ip; }
    }
    CHECK(m1Ip == QStringLiteral("10.0.0.99"));
    CHECK(m2Ip == QStringLiteral("10.0.0.6"));
}

TEST_CASE("an unchanged scan leaves the row identical", "[cstore]") {
    ConnectionStore store(makeSharedSettings());
    store.rememberSatellite(server("m1", "10.0.0.5"));
    const auto before = store.remembered();
    store.refreshFromDiscovery({server("m1", "10.0.0.5")});
    const auto after = store.remembered();
    REQUIRE(before.size() == 1);
    REQUIRE(after.size() == 1);
    CHECK(before[0] == after[0]);
}

// The pin is keyed by IP, so an address change has to migrate it.

TEST_CASE("the cert pin follows the box to a new address", "[cstore]") {
    ConnectionStore store(makeSharedSettings());
    store.rememberSatellite(server("m1", "10.0.0.5"));
    store.pins().pin("10.0.0.5", "fp-original");
    store.refreshFromDiscovery({server("m1", "10.0.0.99")});
    CHECK(store.pins().pinnedFingerprint("10.0.0.99") == QStringLiteral("fp-original"));
    CHECK_FALSE(store.pins().pinnedFingerprint("10.0.0.5").has_value());
}

TEST_CASE("a pin already trusted at the new address is not overwritten", "[cstore]") {
    ConnectionStore store(makeSharedSettings());
    store.rememberSatellite(server("m1", "10.0.0.5"));
    store.pins().pin("10.0.0.5", "fp-old-box");
    store.pins().pin("10.0.0.99", "fp-new-box");
    store.refreshFromDiscovery({server("m1", "10.0.0.99")});
    CHECK(store.pins().pinnedFingerprint("10.0.0.99") == QStringLiteral("fp-new-box"));
    CHECK_FALSE(store.pins().pinnedFingerprint("10.0.0.5").has_value());
}

TEST_CASE("an unchanged address leaves the pin alone", "[cstore]") {
    ConnectionStore store(makeSharedSettings());
    store.rememberSatellite(server("m1", "10.0.0.5"));
    store.pins().pin("10.0.0.5", "fp-original");
    store.refreshFromDiscovery({server("m1", "10.0.0.5")});
    CHECK(store.pins().pinnedFingerprint("10.0.0.5") == QStringLiteral("fp-original"));
}

TEST_CASE("remembering a session at a new address migrates the pin", "[cstore]") {
    ConnectionStore store(makeSharedSettings());
    store.rememberSatellite(server("m1", "10.0.0.5"));
    store.pins().pin("10.0.0.5", "fp-original");
    store.rememberSatellite(server("m1", "10.0.0.42"));
    CHECK(store.pins().pinnedFingerprint("10.0.0.42") == QStringLiteral("fp-original"));
    CHECK_FALSE(store.pins().pinnedFingerprint("10.0.0.5").has_value());
}

TEST_CASE("forget drops the cert pin with the row and the key", "[cstore]") {
    ConnectionStore store(makeSharedSettings());
    store.rememberSatellite(server("m1", "10.0.0.5"));
    store.setSatelliteSharedKey(midId("m1"), kKeyAA);
    store.pins().pin("10.0.0.5", "fp-original");
    store.forgetSatellite(midId("m1"));
    CHECK_FALSE(store.pins().pinnedFingerprint("10.0.0.5").has_value());
    CHECK_FALSE(store.satelliteSharedKey(midId("m1")).has_value());
    CHECK(store.remembered().isEmpty());
}

TEST_CASE("forget of an unknown id leaves other pins alone", "[cstore]") {
    ConnectionStore store(makeSharedSettings());
    store.rememberSatellite(server("m1", "10.0.0.5"));
    store.pins().pin("10.0.0.5", "fp-original");
    store.forgetSatellite(midId("other"));
    CHECK(store.pins().pinnedFingerprint("10.0.0.5") == QStringLiteral("fp-original"));
    CHECK(store.remembered().size() == 1);
}
