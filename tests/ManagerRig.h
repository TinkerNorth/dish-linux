// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The real WifiConnectionManager over a real ConnectionStore, with the
// satellite it talks to on loopback and a server record that points at it.

#pragma once

#include "Network/ConnectionStore.h"
#include "Network/WifiConnectionManager.h"

#include "FakePairingListener.h"
#include "QSettingsFixture.h"

#include <QSettings>
#include <QString>

#include <memory>

namespace dish::test {

// Built in the order AppModel builds them; the settings file outlives the store.
struct ManagerRig {
    FakePairingListener listener;
    std::shared_ptr<QSettings> shared = makeSharedSettings();
    std::unique_ptr<net::ConnectionStore> store;
    std::unique_ptr<net::WifiConnectionManager> wifi;
    models::DiscoveredServer server;

    ManagerRig() {
        store = std::make_unique<net::ConnectionStore>(
            std::unique_ptr<QSettings>(new QSettings(shared->fileName(), QSettings::IniFormat)));
        wifi = std::make_unique<net::WifiConnectionManager>(store.get());
        server.machineId = QStringLiteral("m-den");
        server.ip = QStringLiteral("127.0.0.1");
        server.name = QStringLiteral("Den");
        server.pairPort = listener.port();
        server.httpPort = listener.port();
    }
};

} // namespace dish::test
