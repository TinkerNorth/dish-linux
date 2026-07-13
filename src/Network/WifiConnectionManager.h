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

#include <array>
#include <cstdint>
#include <optional>

namespace dish::net {

enum class ConnectionEventKind { PairingRequired, Error };

struct ConnectionEvent {
    ConnectionEventKind kind;
    models::DiscoveredServer server; // only meaningful for PairingRequired
    QString message;                 // only meaningful for Error
};

// Why this connect was initiated. Drives the toast policy (only user-initiated
// failures are loud) and the backoff bookkeeping (user intent resets it).
// Mirrors dish-android's ConnectIntent / dish-windows' session coordinator.
enum class ConnectIntent { UserInitiated, AutoReconnect, RetryAfterDeath };

// Owns the pool of live + remembered WiFi sessions. Each session runs its own
// native socket, heartbeat and receive loop so multiple servers can be active
// in parallel.
//
// Protocol-1 control plane: sessions open with a declarative
// PUT /api/connections carrying the full desired controller set + the
// hmacProof; the session key is HKDF-derived from the pairing key + the
// response's salt/token (the pairing key never touches UDP). Terminal 401s
// (NOT_PAIRED / BAD_PROOF) and close-notify(unpaired) drop the stored key and
// park the row on "needs pairing"; shutdown/kicked/death re-enter the
// exponential backoff curve consumed by autoReconnectAll().
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
    void connectTo(const models::DiscoveredServer& server,
                   ConnectIntent intent = ConnectIntent::UserInitiated);
    void pairWithPin(const models::DiscoveredServer& server, const QString& pin);
    void disconnect(const QString& id);
    void forget(const QString& id);
    void autoReconnectAll();

    QList<models::RememberedWifi> remembered() const { return store_->remembered(); }

  signals:
    void poolChanged();
    // 1 Hz latency/telemetry relay from the live connections — separate from
    // poolChanged so rows can patch labels in place without a full rebuild.
    void poolTelemetryChanged();
    void discoveredChanged();
    void scanningChanged();
    // Named `connectionEvent` (not `event`) so the signal does not shadow
    // QObject::event(QEvent*), which clang flags with
    // -Wclang-diagnostic-overloaded-virtual.
    void connectionEvent(const dish::net::ConnectionEvent& evt);
    // Forwarded from per-connection WifiConnection::registrationFailed so
    // ConnectionHub can roll back a binding when the server rejects a
    // controller converge.
    void slotRegistrationFailed(const QString& slotId);

  private:
    // Per-connection reconnect throttle (reducer::backoffDelayMs schedule).
    // `suppressed` parks a connection out of auto-reconnect entirely
    // (close-notify(replaced): a newer session owns the satellite — retrying
    // would kick it). User action clears everything.
    struct RetryState {
        int attempt = 0;
        qint64 nextRetryAtMs = 0;
        bool suppressed = false;
    };

    WifiConnection* ensureConnection(const models::DiscoveredServer& server);
    void pairAndConnect(WifiConnection* conn, const models::DiscoveredServer& server,
                        const QString& pin, ConnectIntent intent);
    void openSession(WifiConnection* conn, const models::DiscoveredServer& server,
                     ConnectIntent intent);
    WifiConnection::SessionHooks makeHooks(const QString& id);
    HTTPClient::PinVerifier makePinVerifier() const;

    // The stored pairing key as raw bytes, or nullopt when absent/malformed.
    std::optional<std::array<std::uint8_t, 32>> pairingKeyFor(const QString& id) const;
    // hex(HMAC-SHA256(pairingKey, "satellite-proof:" + deviceId)) for the
    // X-Hmac-Proof header; empty when no key is stored.
    QString proofFor(const QString& id) const;

    // Terminal 401 / close-notify(unpaired): drop ONLY the key (the row
    // survives → LinkState::Stale, "needs pairing"), stop retrying, tear the
    // session. Loud only for user intents.
    void handleTerminalAuth(const QString& id, bool loud);
    void handleDead(const QString& id);
    void handleClose(const QString& id, std::uint8_t reason);
    void runReconcile(const QString& id);
    void scheduleRetry(const QString& id);
    void clearRetry(const QString& id) { retry_.remove(id); }

    ConnectionStore* store_;
    HTTPClient* http_;
    QString deviceId_;
    QString deviceName_;

    QHash<QString, WifiConnection*> connections_;
    QHash<QString, RetryState> retry_;
    QList<models::DiscoveredServer> discovered_;
    bool scanning_ = false;
};

} // namespace dish::net
