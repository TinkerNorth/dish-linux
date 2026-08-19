// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#pragma once

#include "ConnectionStore.h"
#include "HTTPClient.h"
#include "Models/Models.h"
#include "WifiConnection.h"

#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>

#include <array>
#include <cstdint>
#include <optional>

class QTimer;

namespace dish::net {

enum class ConnectionEventKind { PairingRequired, Error };

// Reverse (host-initiated) pairing: the dish shows a clientPin, the operator
// types it on the satellite, and the poll loop resolves. The terminal arms are
// sticky until the next request or cancel clears them.
enum class ReversePairingPhase { Idle, AwaitingApproval, Approved, Declined, TimedOut };

struct ConnectionEvent {
    ConnectionEventKind kind;
    models::DiscoveredServer server; // only meaningful for PairingRequired
    QString message;                 // only meaningful for Error
};

// Only UserInitiated may toast on failure. The two background intents must fail
// silently, with the row chip's Connecting → Saved/Stale flip as the only cue.
enum class ConnectIntent { UserInitiated, AutoReconnect, RetryAfterDeath };

// Owns the pool of live and remembered WiFi sessions and drives the REST control
// plane for each: session PUT on connect, per-controller PUT/DELETE for slot
// changes, the GET-then-rePUT reconcile, terminal-401 handling, close-notify
// teardown, and the reconnect backoff. UDP carries streams only.
class WifiConnectionManager : public QObject {
    Q_OBJECT
  public:
    explicit WifiConnectionManager(ConnectionStore* store, QObject* parent = nullptr);
    ~WifiConnectionManager() override;

    bool isScanning() const { return scanning_; }
    QList<models::DiscoveredServer> discoveredServers() const { return discovered_; }
    const QHash<QString, WifiConnection*>& connections() const { return connections_; }
    WifiConnection* get(const QString& id) const { return connections_.value(id, nullptr); }

    // Main thread only.
    bool isPairingInFlight(const QString& id) const { return pairingInFlight_.contains(id); }

    void startDiscovery();
    void connectTo(const models::DiscoveredServer& server,
                   ConnectIntent intent = ConnectIntent::UserInitiated);
    void pairWithPin(const models::DiscoveredServer& server, const QString& pin);

    // Posts a generated clientPin, then polls /api/pair/status until the operator
    // approves. On approval it adopts the key and opens the session exactly like
    // a forward pair. A second request while one is live cancels the first.
    // Untestable in the unit suite since it drives real network; the decision core
    // it leans on, reducer::nextReversePairingAction, is exhaustively tested.
    void requestReversePairing(const models::DiscoveredServer& server);
    void cancelReversePairing();

    ReversePairingPhase reversePairingPhase() const { return reversePhase_; }
    QString reversePairingPin() const { return reversePin_; }
    QString reversePairingServerName() const { return reverseServerName_; }

    void disconnect(const QString& id);
    void forget(const QString& id);
    void autoReconnectAll();

    QList<models::RememberedWifi> remembered() const { return store_->remembered(); }

  signals:
    void poolChanged();
    // Kept separate from poolChanged so the hub and AppModel rebuild cascade never
    // runs for a cosmetic 1 Hz figure.
    void poolTelemetryChanged();
    void discoveredChanged();
    void scanningChanged();
    void pairingInFlightChanged();
    void reversePairingChanged();
    // Not named `event`, which would shadow QObject::event.
    void connectionEvent(const dish::net::ConnectionEvent& evt);
    // Lets the hub roll a binding back when the server rejects a descriptor.
    void slotRegistrationFailed(const QString& slotId);
    // A rejected FORWARD pair, with `reasonToken` one of "wrongPin" |
    // "versionMismatch" | "unreachable" | "pending". Separate from the toast so
    // the pairing sheet can stay open and mark the field inline. Deliberately not
    // raised from pairAndConnect: a background reconnect must never pop an error
    // into a sheet the user did not open.
    void pairingFailed(const QString& connectionId, const QString& reasonToken);

  private:
    WifiConnection* ensureConnection(const models::DiscoveredServer& server);
    void wireSlotSync(WifiConnection* conn);
    void pairAndConnect(WifiConnection* conn, const models::DiscoveredServer& server,
                        ConnectIntent intent);
    // One PUT /api/connections carrying identity, the key proof and the FULL
    // topology, which is what drives the session live.
    void openSession(WifiConnection* conn, const models::DiscoveredServer& server,
                     ConnectIntent intent);
    // GET-then-maybe-rePUT, fired when the enriched ack drifts.
    void reconcile(WifiConnection* conn, const models::DiscoveredServer& server);
    // Re-PUT for a fresh token/salt/key on the SAME socket, so there is no state
    // blip visible to the UI.
    void rekey(WifiConnection* conn, const models::DiscoveredServer& server);
    void syncSlot(const QString& id, const QString& slotId);
    void deleteSlot(const QString& id, int ctrlIdx);
    void handleServerClose(WifiConnection* conn, const models::DiscoveredServer& server,
                           std::uint8_t reason);
    // Never runs for UserInitiated; a user tap resets the curve.
    void scheduleRetry(const models::DiscoveredServer& server, ConnectIntent intent);

    void emitErrorIfUserInitiated(ConnectIntent intent, const QString& message);
    void markStale(const QString& id);

    // One pairStatus round-trip off the thread pool, fed with the elapsed clock
    // through reducer::nextReversePairingAction to decide re-arm / open / abort.
    void pollReverseStatus();
    void setReversePhase(ReversePairingPhase phase);
    void finishReverse(ReversePairingPhase terminal);

    // nullopt when the stored key is absent or undecodable, both of which mean
    // re-pair.
    struct Credentials {
        std::array<std::uint8_t, 32> pairingKey{};
        QString proof;
    };
    std::optional<Credentials> credentialsFor(const QString& id) const;
    // Drops the key, parks Stale and stops retrying. Centralised so every REST
    // path treats a terminal auth failure identically.
    void onTerminalAuthFailure(WifiConnection* conn, const QString& id, ConnectIntent intent);

    ConnectionStore* store_;
    HTTPClient* http_;
    QString deviceId_;
    QString deviceName_;

    QHash<QString, WifiConnection*> connections_;
    QList<models::DiscoveredServer> discovered_;
    bool scanning_ = false;
    QSet<QString> pairingInFlight_;
    // Drives the backoff. Reset on a successful session or any user action.
    QHash<QString, int> retryAttempts_;
    // Single-flight guard: the ack ticks every second but the GET can take longer.
    QSet<QString> reconcileInFlight_;

    ReversePairingPhase reversePhase_ = ReversePairingPhase::Idle;
    QString reversePin_;
    QString reverseServerName_;
    models::DiscoveredServer reverseServer_;
    QTimer* reverseTimer_ = nullptr;
    std::int64_t reverseElapsedMs_ = 0;
    std::int64_t reverseDeadlineMs_ = 0;
    // Disambiguates a "none" reply: after a pending, it means the operator's deny
    // erased the row and is terminal; before one, it is just the POST-to-first-poll
    // race and is tolerated. Reset per attempt.
    bool reverseSawPending_ = false;
    bool reversePollInFlight_ = false;
};

} // namespace dish::net
