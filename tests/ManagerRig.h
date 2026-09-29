// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The real WifiConnectionManager over a real ConnectionStore, with the
// satellite it talks to on loopback and a server record that points at it.

#pragma once

#include "Network/ConnectionStore.h"
#include "Network/WifiConnectionManager.h"
#include "core/model/Protocol.h"

#include "FakePairingListener.h"
#include "QSettingsFixture.h"

#include <QHostAddress>
#include <QJsonObject>
#include <QObject>
#include <QSettings>
#include <QString>

#include <memory>
#include <vector>

namespace dish::test {

// A pairing key in the shape the store accepts: 32 bytes, as 64 hex digits.
inline const QString kFixtureSharedKey =
    QStringLiteral("a1b2c3d4e5f6a7b8c9d0e1f2a3b4c5d6e7f8a9b0c1d2e3f4a5b6c7d8e9f0a1b2");

// A satellite that grants everything: a pair gets the key, and anything else, a session PUT
// first of all, gets a session.
inline PairingAnswer grantingEverything(const SeenRequest& r) {
    if (r.path == QStringLiteral("/api/pair")) {
        return PairingAnswer{200, QJsonObject{{QStringLiteral("ok"), true},
                                              {QStringLiteral("sharedKey"), kFixtureSharedKey}}};
    }
    return PairingAnswer{
        200, QJsonObject{{QStringLiteral("connectionId"), QStringLiteral("c-1")},
                         {QStringLiteral("token"), QStringLiteral("0a0b0c0d")},
                         {QStringLiteral("sessionSalt"), QStringLiteral("0102030405060708")},
                         {QStringLiteral("epoch"), 1},
                         {QStringLiteral("protocolVersion"), proto::kProtocolVersion}}};
}

// A loopback address as a server record carries it: an IPv6 literal goes in the URL's brackets.
inline QString serverIpFor(const QHostAddress& loopback) {
    const QString literal = loopback.toString();
    const bool isIpv6 = loopback.protocol() == QHostAddress::IPv6Protocol;
    return isIpv6 ? QStringLiteral("[%1]").arg(literal) : literal;
}

// Built in the order AppModel builds them; the settings file outlives the store.
struct ManagerRig {
    FakePairingListener listener;
    std::shared_ptr<QSettings> shared = makeSharedSettings();
    std::unique_ptr<net::ConnectionStore> store;
    std::unique_ptr<net::WifiConnectionManager> wifi;
    models::DiscoveredServer server;
    // Every event the manager raised, in order.
    std::vector<net::ConnectionEvent> events;

    // The satellite answers on IPv4 loopback unless a test needs the other one.
    explicit ManagerRig(QHostAddress::SpecialAddress loopback = QHostAddress::LocalHost)
        : listener(QHostAddress(loopback)) {
        store = std::make_unique<net::ConnectionStore>(
            std::unique_ptr<QSettings>(new QSettings(shared->fileName(), QSettings::IniFormat)));
        wifi = std::make_unique<net::WifiConnectionManager>(store.get());
        QObject::connect(wifi.get(), &net::WifiConnectionManager::connectionEvent, wifi.get(),
                         [this](const net::ConnectionEvent& e) { events.push_back(e); });
        server.machineId = QStringLiteral("m-den");
        server.ip = serverIpFor(QHostAddress(loopback));
        server.name = QStringLiteral("Den");
        server.pairPort = listener.port();
        server.httpPort = listener.port();
    }

    // Whether the store lists the rig's satellite among the remembered ones.
    bool remembered() const {
        for (const auto& r : wifi->remembered()) {
            if (r.id == server.id()) { return true; }
        }
        return false;
    }

    int errors() const {
        int n = 0;
        for (const auto& e : events) {
            if (e.kind == net::ConnectionEventKind::Error) { ++n; }
        }
        return n;
    }
};

} // namespace dish::test
