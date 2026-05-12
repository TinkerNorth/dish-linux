// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#pragma once

#include "ConnectionStore.h"
#include "HTTPClient.h"
#include "Models/Models.h"
#include "WifiConnection.h"

#include <QHash>
#include <QObject>
#include <QString>

namespace dish::net {

enum class ConnectionEventKind { PairingRequired, Error };

struct ConnectionEvent {
    ConnectionEventKind kind;
    models::DiscoveredServer server; // only meaningful for PairingRequired
    QString message;                 // only meaningful for Error
};

// Owns the pool of live + remembered WiFi sessions. Each session runs its own
// native socket, heartbeat and ACK loop so multiple servers can be active in
// parallel. Mirrors dish-mac/Network/WifiConnectionManager.swift.
class WifiConnectionManager : public QObject {
    Q_OBJECT
  public:
    explicit WifiConnectionManager(ConnectionStore* store, QObject* parent = nullptr);
    ~WifiConnectionManager() override;

    bool isScanning() const { return scanning_; }
    QList<models::DiscoveredServer> discoveredServers() const { return discovered_; }
    const QHash<QString, WifiConnection*>& connections() const { return connections_; }
    WifiConnection* get(const QString& id) const { return connections_.value(id, nullptr); }

    void startDiscovery();
    void connectTo(const models::DiscoveredServer& server);
    void pairWithPin(const models::DiscoveredServer& server, const QString& pin);
    void disconnect(const QString& id);
    void forget(const QString& id);
    void autoReconnectAll();

    QList<models::RememberedWifi> remembered() const { return store_->remembered(); }

  signals:
    void poolChanged();
    void discoveredChanged();
    void scanningChanged();
    // Named `connectionEvent` (not `event`) so the signal does not shadow
    // QObject::event(QEvent*), which clang flags with
    // -Wclang-diagnostic-overloaded-virtual.
    void connectionEvent(const dish::net::ConnectionEvent& evt);
    // Forwarded from per-connection WifiConnection::registrationFailed so
    // ConnectionHub can roll back a binding when the server rejects a
    // controller add.
    void slotRegistrationFailed(const QString& slotId);

  private:
    WifiConnection* ensureConnection(const models::DiscoveredServer& server);
    void pairAndConnect(WifiConnection* conn, const models::DiscoveredServer& server,
                        const QString& pin);
    void openSession(WifiConnection* conn, const models::DiscoveredServer& server);

    ConnectionStore* store_;
    HTTPClient* http_;
    QString deviceId_;
    QString deviceName_;

    QHash<QString, WifiConnection*> connections_;
    QList<models::DiscoveredServer> discovered_;
    bool scanning_ = false;
};

} // namespace dish::net
