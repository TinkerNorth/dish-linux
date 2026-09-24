// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#pragma once

#include "ConnectionStore.h"
#include "HTTPClient.h"
#include "Models/Models.h"
#include "PairingClient.h"
#include "WifiConnection.h"
#include "core/reducer/RestOutcome.h"
#include "core/reducer/ReversePairing.h"
#include "core/wire/SessionCrypto.h"

#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>

#include <array>
#include <cstdint>
#include <optional>

class QTimer;

namespace dish::net {

enum class ConnectionEventKind : std::uint8_t { PairingRequired, Error };

// Reverse (host-initiated) pairing: the dish shows a clientPin, the operator
// types it on the satellite, and the poll loop resolves. The terminal arms are
// sticky until the next request or cancel clears them.
enum class ReversePairingPhase : std::uint8_t {
    Idle,
    AwaitingApproval,
    Approved,
    Declined,
    TimedOut
};

struct ConnectionEvent {
    ConnectionEventKind kind;
    models::DiscoveredServer server; // only meaningful for PairingRequired
    QString message;                 // only meaningful for Error
};

// Only UserInitiated may toast on failure. The two background intents must fail
// silently, with the row chip's Connecting → Saved/Stale flip as the only cue.
enum class ConnectIntent : std::uint8_t { UserInitiated, AutoReconnect, RetryAfterDeath };

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
    // Driven end to end in test_reverse_pairing_flow against a loopback TLS listener; the decision
    // core it leans on, reducer::nextReversePairingAction, is tested on its own as well.
    void requestReversePairing(const models::DiscoveredServer& server);
    void cancelReversePairing();

    ReversePairingPhase reversePairingPhase() const { return reversePhase_; }
    QString reversePairingPin() const { return reversePin_; }
    QString reversePairingServerName() const { return reverseServerName_; }

    void disconnect(const QString& id);
    void forget(const QString& id);
    void autoReconnectAll();

    // Suspend/resume. Tearing down first matters: a session the machine slept
    // through comes back Faltering, which slips past both autoReconnectAll's
    // not-Live test and connectTo's Live-or-Linking guard, so a bare reconnect
    // would open a second socket beside the frozen one and strand it.
    void prepareForSleep();
    void resumeFromSleep();

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

    // The rekey PUT's reply, in three steps: is this still the session that asked, does the reply
    // carry material, and adopt it. The same steps dish-windows names the same way.
    struct RekeyMaterial {
        std::array<std::uint8_t, 4> token{};
        std::array<std::uint8_t, wire::kSessionSaltSize> salt{};
        std::uint32_t tokenBe = 0;
    };
    static std::optional<RekeyMaterial> rekeyMaterialFrom(const models::SessionResponse& resp);
    void onRekeyReply(const QString& id, const std::shared_ptr<SatelliteClient>& client,
                      const std::array<std::uint8_t, 32>& pairingKey,
                      const models::SessionResponse& resp, bool pinMismatch);
    void adoptRekey(WifiConnection* c, const QString& id,
                    const std::shared_ptr<SatelliteClient>& client,
                    const std::array<std::uint8_t, 32>& pairingKey,
                    const models::SessionResponse& resp, const RekeyMaterial& material);
    // Reads GET /api/server/capabilities for the host's controller-audio
    // verdict and folds it into the connection (reducer/HostAudioVerdict.h).
    // Fired after EVERY successful session PUT — connect, reconnect-after-death
    // and the proactive re-key alike — because the verdict is live host state
    // and a fresh session is exactly when it may have moved. Probing only from
    // a UI surface was dish-android's mistake: an auto-reconnected session then
    // streams with the verdict stuck at the conservative default.
    void probeHostAudio(const QString& id, const models::DiscoveredServer& server);
    void syncSlot(const QString& id, const QString& slotId);
    void deleteSlot(const QString& id, int ctrlIdx);
    void handleServerClose(WifiConnection* conn, const models::DiscoveredServer& server,
                           std::uint8_t reason);
    // Never runs for UserInitiated; a user tap resets the curve.
    // `failure` is diagnostic only: it selects what gets logged, never whether
    // or how long we retry. The curve stays a pure function of the attempt.
    void scheduleRetry(const models::DiscoveredServer& server, ConnectIntent intent,
                       reducer::TransportFailure failure = reducer::TransportFailure::None);

    void emitErrorIfUserInitiated(ConnectIntent intent, const QString& message);
    void markStale(const QString& id);

    // One pairStatus round-trip off the thread pool, fed with the elapsed clock
    // through reducer::nextReversePairingAction to decide re-arm / open / abort.
    void pollReverseStatus();

    // Reverse pairing in order: arm an attempt, send it, read the reply, and poll for the
    // operator's answer. The same names dish-windows uses for the same steps.
    static QString drawReversePin();
    void armReverseAttempt(const models::DiscoveredServer& server);
    bool reverseAttemptIsCurrent(const models::DiscoveredServer& server, const QString& pin) const;
    void startReversePoll();
    void applyReverseOutcome(WifiConnection* conn, const models::DiscoveredServer& server,
                             const PairingClient::Reply& pair);
    void onReversePairReply(WifiConnection* conn, const models::DiscoveredServer& server,
                            const QString& pin, const PairingClient::Reply& pair);
    static reducer::ApprovalReply approvalReplyOf(const models::PairResponse& status);
    void applyReverseAction(reducer::ReversePairingAction action,
                            const PairingClient::Reply& status,
                            const models::DiscoveredServer& server);
    void onReverseStatusReply(const PairingClient::Reply& status,
                              const models::DiscoveredServer& server);
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
    // Built in the constructor over the same pin store as http_, then copied into
    // each pairing worker. Empty (accepting) only until the constructor runs.
    PairingClient pairing_{PairingClient::PinVerifier{}};
    QString deviceId_;
    QString deviceName_;

    QHash<QString, WifiConnection*> connections_;
    QList<models::DiscoveredServer> discovered_;
    bool scanning_ = false;
    QSet<QString> pairingInFlight_;
    // Drives the backoff. Reset on a successful session or any user action.
    QHash<QString, int> retryAttempts_;
    // Last logged cause per satellite, so a box that stays off does not write
    // one identical line per backoff tick. Cleared whenever the id settles.
    QHash<QString, reducer::TransportFailure> lastFailure_;
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
