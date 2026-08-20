// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "WifiConnectionManager.h"

#include "PairingClient.h"
#include "core/net/IpLiterals.h"
#include "source/connection/DiscoveryGateway.h"
#include "source/connection/LANDiscovery.h"
#include "source/connection/MdnsDiscovery.h"
#include "source/http/SatelliteTlsVerifier.h"
#include "Util/Hex.h"
#include "core/reducer/Backoff.h"
#include "core/reducer/CloseNotify.h"
#include "core/reducer/Reconcile.h"
#include "core/reducer/RestOutcome.h"
#include "core/reducer/ReversePairing.h"
#include "core/wire/SessionCrypto.h"

#include <QCoreApplication>
#include <QHostInfo>
#include <QSet>
#include <QSignalBlocker>
#include <QTimer>
#include <QtConcurrent/QtConcurrent>
#include <QtGlobal>

#include <random>
#include <type_traits>
#include <variant>

namespace dish::net {

namespace {

// Pins every user-facing string in this file to one .ts <context> entry.
constexpr const char* kTrContext = "dish::net::WifiConnectionManager";

ConnectionEvent makeError(const QString& msg) { return {ConnectionEventKind::Error, {}, msg}; }

ConnectionEvent pairingRequired(const models::DiscoveredServer& s) {
    return {ConnectionEventKind::PairingRequired, s, {}};
}

// Lives here rather than in core/reducer because it is the Qt-to-pure boundary.
std::vector<reducer::DesiredSlot>
descriptorsToDesired(const QList<models::ControllerDescriptor>& descriptors) {
    std::vector<reducer::DesiredSlot> out;
    out.reserve(static_cast<std::size_t>(descriptors.size()));
    for (const auto& d : descriptors) {
        out.push_back({static_cast<std::uint8_t>(d.ctrlIdx), d.type});
    }
    return out;
}

QString unreachableMsg() {
    return QCoreApplication::translate(
        kTrContext, "Server unreachable — check it's powered on and on the same Wi-Fi.");
}
QString rePairMsg() {
    return QCoreApplication::translate(
        kTrContext, "This satellite no longer recognizes this device. Re-pair needed.");
}
QString versionMsg() {
    return QCoreApplication::translate(
        kTrContext, "This app and the satellite speak different protocol versions.");
}
QString identityChangedMsg() {
    return QCoreApplication::translate(
        kTrContext,
        "This satellite's security identity changed. If it was reinstalled, forget it here and "
        "pair again.");
}
QString wrongPinMsg() {
    return QCoreApplication::translate(
        kTrContext, "That PIN wasn't accepted. Check the code on the satellite and try again.");
}
QString pairPendingMsg() {
    return QCoreApplication::translate(
        kTrContext, "The satellite hasn't confirmed pairing yet. Try again in a moment.");
}
QString reverseDeclinedMsg() {
    return QCoreApplication::translate(
        kTrContext, "The satellite declined this device. Pairing was not approved.");
}
QString reverseTimedOutMsg() {
    return QCoreApplication::translate(
        kTrContext, "Timed out waiting for approval on the satellite. Try again.");
}

// The window an operator needs to read the PIN and approve on the satellite. The
// elapsed clock accumulates from these intervals rather than a wall-clock read,
// so the pure decision is driven by integers the manager fully controls.
constexpr int kReversePollIntervalMs = 1000;
constexpr std::int64_t kReverseDeadlineMs = 120'000;

} // namespace

WifiConnectionManager::WifiConnectionManager(ConnectionStore* store, QObject* parent)
    : QObject(parent), store_(store), http_(new HTTPClient(this)) {
    deviceId_ = store_->getOrCreateDeviceId();
    deviceName_ = QHostInfo::localHostName();
    if (deviceName_.isEmpty()) { deviceName_ = QStringLiteral("Linux"); }
    // TOFU on every HTTPS call, keyed by host to match the ConnectionStore
    // pin-migration convention. `pins` is captured by reference below, so the
    // store must outlive this manager.
    auto& pins = store_->facade().pins();
    // Reporting the mismatch per request is the whole point: a changed cert and a
    // dead link both abort the transfer, and only this flag tells them apart.
    auto verify = [&pins](const QString& host, const QByteArray& certDer, bool& pinMismatch) {
        return http::verifyPeerCertificate(host, pins, certDer,
                                           [&pinMismatch] { pinMismatch = true; });
    };
    http_->setPinVerifier(verify);
    // The same gate over the same store, so the first pair pins and every later
    // pairing or rotation must present the pinned cert.
    PairingClient::setPinVerifier(verify);
}

WifiConnectionManager::~WifiConnectionManager() {
    // This loop exists to tear down live sessions, not to announce anything —
    // and by the time it runs there is nobody left who can safely listen.
    //
    // The manager is a QObject CHILD of AppModel, so it is deleted from
    // ~QObject's deleteChildren(), which is after every AppModel member has
    // already been destroyed — ConnectionStore among them. markDisconnected()
    // emits WifiConnection::changed, the manager relays it as poolChanged, and
    // ConnectionHub::rebuild() then reads through the store's freed
    // unique_ptr<RememberedSatelliteRepository>. That was an access violation on
    // every single exit (0xC0000005, crash.dmp written by the handler, so it
    // looked like a clean quit from outside).
    //
    // Blocking the source signal is the fix that does not depend on which
    // collaborator happens to die first. ~WifiConnection blocks for itself too,
    // which is what covers the connections this loop cannot see: forget() takes
    // one out of the map and leaves it on deleteLater, still a child of this
    // manager and still destroyed from the same deleteChildren() pass.
    for (auto* c : connections_) {
        const QSignalBlocker block(c);
        c->markDisconnected();
    }
}

void WifiConnectionManager::startDiscovery() {
    if (scanning_) { return; }
    scanning_ = true;
    emit scanningChanged();
    auto* watcher = new QFutureWatcher<QList<models::DiscoveredServer>>(this);
    QObject::connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher] {
        discovered_ = watcher->result();
        scanning_ = false;
        // Persist a moved satellite's new IP BEFORE anything else, so the next
        // launch's autoReconnectAll and any in-flight backoff retry (which
        // re-reads store_->remembered()) target the current address. The only
        // other path that writes a fresh IP is a successful session PUT, which
        // cannot happen while the IP is wrong: that is the "must rescan, then
        // reconnect" trap.
        store_->refreshFromDiscovery(discovered_);
        // The same relearn for the in-memory connection.
        for (const auto& server : discovered_) {
            if (auto* conn = connections_.value(server.id(), nullptr)) {
                if (conn->state() == SessionState::Idle || conn->state() == SessionState::Stale) {
                    conn->updateServer(server);
                }
            }
        }
        // So a moved box reconnects on its own once the scan finds it, with no
        // manual Connect.
        autoReconnectAll();
        emit discoveredChanged();
        emit scanningChanged();
        // No "found nothing" event is emitted: an empty discovered_ with
        // scanning_ false IS that state, and the page binds it directly.
        watcher->deleteLater();
    });
    watcher->setFuture(QtConcurrent::run([] {
        auto mdnsFuture = QtConcurrent::run([] { return MdnsDiscovery::discover(); });
        const QList<models::DiscoveredServer> beacon = LANDiscovery::discover();
        const QList<models::DiscoveredServer> mdns = mdnsFuture.result();
        const QList<models::DiscoveredServer> merged =
            DiscoveryGateway::mergeDiscovered(beacon, mdns);
        qInfo("discovery scan: broadcast=%lld mdns=%lld merged=%lld",
              static_cast<long long>(beacon.size()), static_cast<long long>(mdns.size()),
              static_cast<long long>(merged.size()));
        return merged;
    }));
}

void WifiConnectionManager::wireSlotSync(WifiConnection* conn) {
    const QString id = conn->id();
    // Converging via the per-controller routes keeps the session and UDP keys
    // from churning over a toggle.
    QObject::connect(conn, &WifiConnection::slotChanged, this,
                     [this, id](const QString& slotId) { syncSlot(id, slotId); });
    QObject::connect(conn, &WifiConnection::slotRemoved, this,
                     [this, id](int ctrlIdx) { deleteSlot(id, ctrlIdx); });
}

WifiConnection* WifiConnectionManager::ensureConnection(const models::DiscoveredServer& server) {
    const auto id = WifiConnection::idFor(server);
    if (auto* existing = connections_.value(id, nullptr)) { return existing; }
    auto* conn = new WifiConnection(id, server, this);
    connections_.insert(id, conn);
    QObject::connect(conn, &WifiConnection::changed, this, &WifiConnectionManager::poolChanged);
    QObject::connect(conn, &WifiConnection::telemetryChanged, this,
                     &WifiConnectionManager::poolTelemetryChanged);
    QObject::connect(conn, &WifiConnection::errorOccurred, this,
                     [this](const QString& msg) { emit connectionEvent(makeError(msg)); });
    wireSlotSync(conn);
    emit poolChanged();
    return conn;
}

std::optional<WifiConnectionManager::Credentials>
WifiConnectionManager::credentialsFor(const QString& id) const {
    const auto keyHex = store_->sharedKey(id);
    if (!keyHex.has_value() || keyHex->size() != 64) { return std::nullopt; }
    const auto keyBytes = util::fromHex(keyHex->toStdString());
    if (!keyBytes || keyBytes->size() != 32) { return std::nullopt; }
    Credentials creds;
    std::copy_n(keyBytes->begin(), 32, creds.pairingKey.begin());
    creds.proof = QString::fromStdString(
        wire::computeHmacProof(creds.pairingKey.data(), deviceId_.toStdString()));
    return creds;
}

void WifiConnectionManager::connectTo(const models::DiscoveredServer& server,
                                      ConnectIntent intent) {
    // Satellites are LAN-only by definition, so a public literal here means a
    // spoofed beacon or a poisoned remembered entry. Dialing it would leak the
    // deviceId and hmacProof to an arbitrary internet host.
    if (!isPrivateHostLiteral(server.ip.toStdString())) {
        emit connectionEvent(
            makeError(tr("Refusing to connect to a non-local address (%1).").arg(server.ip)));
        return;
    }
    auto* conn = ensureConnection(server);
    if (intent == ConnectIntent::UserInitiated) { retryAttempts_.remove(conn->id()); }
    if (conn->state() == SessionState::Live || conn->state() == SessionState::Linking) {
        conn->updateServer(server);
        return;
    }
    conn->updateServer(server);
    conn->markConnecting();
    // With a key in hand, skip the pair handshake so a moved or offline server
    // fails fast in the session PUT instead of bouncing through PairingRequired
    // and trapping the user behind a PIN prompt.
    if (credentialsFor(conn->id()).has_value()) {
        openSession(conn, server, intent);
    } else {
        pairAndConnect(conn, server, intent);
    }
}

void WifiConnectionManager::pairWithPin(const models::DiscoveredServer& server,
                                        const QString& pin) {
    auto* conn = ensureConnection(server);
    retryAttempts_.remove(conn->id());
    if (conn->state() == SessionState::Live) { return; }
    conn->updateServer(server);
    conn->markConnecting();

    const QString did = deviceId_;
    const QString dname = deviceName_;
    pairingInFlight_.insert(conn->id());
    emit pairingInFlightChanged();
    auto* watcher = new QFutureWatcher<PairingClient::Reply>(this);
    QObject::connect(
        watcher, &QFutureWatcherBase::finished, this, [this, watcher, conn, server, pin] {
            const auto pair = watcher->result();
            watcher->deleteLater();
            pairingInFlight_.remove(conn->id());
            emit pairingInFlightChanged();
            const auto outcome = PairingClient::classify(pair.response, pair.pinMismatch);
            std::visit(
                [&](auto&& arm) {
                    using T = std::decay_t<decltype(arm)>;
                    if constexpr (std::is_same_v<T, PairingClient::Success>) {
                        store_->setSharedKey(arm.sharedKeyHex, WifiConnection::idFor(server));
                        openSession(conn, server, ConnectIntent::UserInitiated);
                    } else if constexpr (std::is_same_v<T, PairingClient::IdentityChanged>) {
                        // No pairingFailed: every reasonToken the sheet knows would
                        // blame the PIN or the network. The error channel already
                        // clears its submitting state, and it carries the real cue.
                        conn->markDisconnected();
                        emit connectionEvent(makeError(identityChangedMsg()));
                    } else if constexpr (std::is_same_v<T, PairingClient::VersionMismatch>) {
                        conn->markDisconnected();
                        emit connectionEvent(makeError(versionMsg()));
                        emit pairingFailed(conn->id(), QStringLiteral("versionMismatch"));
                    } else if constexpr (std::is_same_v<T, PairingClient::AuthRequired>) {
                        // Reachable and parsed but no key granted, so the PIN was
                        // wrong or expired.
                        conn->markDisconnected();
                        emit connectionEvent(makeError(wrongPinMsg()));
                        emit pairingFailed(conn->id(), QStringLiteral("wrongPin"));
                    } else if constexpr (std::is_same_v<T, PairingClient::Unreachable>) {
                        conn->markDisconnected();
                        emit connectionEvent(makeError(unreachableMsg()));
                        emit pairingFailed(conn->id(), QStringLiteral("unreachable"));
                    } else {
                        // Pending: staged but not granted, rare on a direct submit.
                        conn->markDisconnected();
                        emit connectionEvent(makeError(pairPendingMsg()));
                        emit pairingFailed(conn->id(), QStringLiteral("pending"));
                    }
                },
                outcome);
        });
    watcher->setFuture(QtConcurrent::run([server, did, dname, pin] {
        return PairingClient::pair(server.ip, server.pairPort, did, dname, pin);
    }));
}

void WifiConnectionManager::requestReversePairing(const models::DiscoveredServer& server) {
    // A fresh request supersedes any in-flight one and clears a previous
    // attempt's terminal arm.
    cancelReversePairing();

    auto* conn = ensureConnection(server);
    retryAttempts_.remove(conn->id());
    conn->updateServer(server);

    // The value is random but the shape is fixed by the pure formatter, so the
    // displayed PIN is always exactly 4 digits. Randomness stays out of the
    // tested decision core.
    std::random_device rd;
    const std::uint32_t draw = rd();
    reversePin_ = QString::fromStdString(reducer::formatReversePin(draw));
    reverseServer_ = server;
    reverseServerName_ = server.name.isEmpty() ? server.ip : server.name;
    reverseElapsedMs_ = 0;
    reverseDeadlineMs_ = kReverseDeadlineMs;
    reverseSawPending_ = false;
    setReversePhase(ReversePairingPhase::AwaitingApproval);

    const QString did = deviceId_;
    const QString dname = deviceName_;
    const QString pin = reversePin_;
    // The happy-path reply is {ok:false, pending:true}, which then gets polled.
    pairingInFlight_.insert(conn->id());
    emit pairingInFlightChanged();
    auto* watcher = new QFutureWatcher<PairingClient::Reply>(this);
    QObject::connect(
        watcher, &QFutureWatcherBase::finished, this, [this, watcher, conn, server, pin] {
            const auto pair = watcher->result();
            watcher->deleteLater();
            pairingInFlight_.remove(conn->id());
            emit pairingInFlightChanged();
            // A cancel or restart landed while this POST was in flight; drop the
            // late reply rather than polling for a superseded request.
            if (reversePhase_ != ReversePairingPhase::AwaitingApproval ||
                reverseServer_.id() != server.id() || reversePin_ != pin) {
                return;
            }
            const auto outcome = PairingClient::classify(pair.response, pair.pinMismatch);
            std::visit(
                [&](auto&& arm) {
                    using T = std::decay_t<decltype(arm)>;
                    if constexpr (std::is_same_v<T, PairingClient::Success>) {
                        // Approved synchronously, with no operator step.
                        conn->markConnecting();
                        store_->setSharedKey(arm.sharedKeyHex, WifiConnection::idFor(server));
                        setReversePhase(ReversePairingPhase::Approved);
                        openSession(conn, server, ConnectIntent::UserInitiated);
                    } else if constexpr (std::is_same_v<T, PairingClient::Pending>) {
                        // The expected arm: start the approval poll loop.
                        if (reverseTimer_ == nullptr) {
                            reverseTimer_ = new QTimer(this);
                            reverseTimer_->setInterval(kReversePollIntervalMs);
                            QObject::connect(reverseTimer_, &QTimer::timeout, this,
                                             &WifiConnectionManager::pollReverseStatus);
                        }
                        reverseTimer_->start();
                    } else if constexpr (std::is_same_v<T, PairingClient::VersionMismatch>) {
                        emit connectionEvent(makeError(versionMsg()));
                        finishReverse(ReversePairingPhase::Declined);
                    } else if constexpr (std::is_same_v<T, PairingClient::IdentityChanged>) {
                        emit connectionEvent(makeError(identityChangedMsg()));
                        finishReverse(ReversePairingPhase::Declined);
                    } else {
                        // AuthRequired or Unreachable: no pending grant was staged.
                        emit connectionEvent(
                            makeError(pair.response.error.value_or(unreachableMsg())));
                        finishReverse(ReversePairingPhase::TimedOut);
                    }
                },
                outcome);
        });
    watcher->setFuture(QtConcurrent::run([server, did, dname, pin] {
        // Empty operator pin, displayed pin as clientPin: that is what selects
        // Path B server-side.
        return PairingClient::pair(server.ip, server.pairPort, did, dname, QString(), pin);
    }));
}

void WifiConnectionManager::pollReverseStatus() {
    if (reversePhase_ != ReversePairingPhase::AwaitingApproval) { return; }
    // A slow GET must not stack behind the 1 s timer.
    if (reversePollInFlight_) { return; }
    reversePollInFlight_ = true;
    reverseElapsedMs_ += kReversePollIntervalMs;

    const QString did = deviceId_;
    const models::DiscoveredServer server = reverseServer_;
    auto* watcher = new QFutureWatcher<PairingClient::Reply>(this);
    QObject::connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, server] {
        const auto status = watcher->result();
        watcher->deleteLater();
        reversePollInFlight_ = false;
        // A cancel or restart raced this GET, so its reply is superseded.
        if (reversePhase_ != ReversePairingPhase::AwaitingApproval ||
            reverseServer_.id() != server.id()) {
            return;
        }
        // Terminal, and ahead of the approval ladder: polling on would just spend
        // the operator's whole window against a box we can no longer authenticate.
        if (status.pinMismatch) {
            emit connectionEvent(makeError(identityChangedMsg()));
            finishReverse(ReversePairingPhase::Declined);
            return;
        }
        reducer::ApprovalReply ar;
        ar.status = status.response.httpStatus;
        ar.bodyParsed = status.response.reachable;
        ar.statusStr = status.response.status.value_or(QString()).toStdString();
        ar.hasSharedKey =
            status.response.sharedKey.has_value() && !status.response.sharedKey->isEmpty();
        const auto approval = reducer::classifyApproval(ar, reverseSawPending_);
        if (ar.statusStr == "pending") { reverseSawPending_ = true; }
        switch (
            reducer::nextReversePairingAction(approval, reverseElapsedMs_, reverseDeadlineMs_)) {
        case reducer::ReversePairingAction::Approve: {
            auto* conn = ensureConnection(server);
            conn->markConnecting();
            store_->setSharedKey(*status.response.sharedKey, WifiConnection::idFor(server));
            if (reverseTimer_ != nullptr) { reverseTimer_->stop(); }
            setReversePhase(ReversePairingPhase::Approved);
            openSession(conn, server, ConnectIntent::UserInitiated);
            break;
        }
        case reducer::ReversePairingAction::Decline:
            emit connectionEvent(makeError(reverseDeclinedMsg()));
            finishReverse(ReversePairingPhase::Declined);
            break;
        case reducer::ReversePairingAction::TimeOut:
            emit connectionEvent(makeError(reverseTimedOutMsg()));
            finishReverse(ReversePairingPhase::TimedOut);
            break;
        case reducer::ReversePairingAction::KeepPolling:
            break; // the timer re-fires on its own
        }
    });
    watcher->setFuture(QtConcurrent::run(
        [server, did] { return PairingClient::pairStatus(server.ip, server.pairPort, did); }));
}

void WifiConnectionManager::cancelReversePairing() {
    if (reverseTimer_ != nullptr) { reverseTimer_->stop(); }
    reversePollInFlight_ = false;
    if (reversePhase_ != ReversePairingPhase::Idle) {
        reversePin_.clear();
        reverseServerName_.clear();
        reverseServer_ = {};
        reverseElapsedMs_ = 0;
        setReversePhase(ReversePairingPhase::Idle);
    }
}

void WifiConnectionManager::finishReverse(ReversePairingPhase terminal) {
    if (reverseTimer_ != nullptr) { reverseTimer_->stop(); }
    reversePollInFlight_ = false;
    // The pin and server name survive the terminal arm so the sheet can still
    // name what it was pairing; the next request or cancel clears them.
    setReversePhase(terminal);
}

void WifiConnectionManager::setReversePhase(ReversePairingPhase phase) {
    reversePhase_ = phase;
    emit reversePairingChanged();
}

void WifiConnectionManager::pairAndConnect(WifiConnection* conn,
                                           const models::DiscoveredServer& server,
                                           ConnectIntent intent) {
    const QString did = deviceId_;
    const QString dname = deviceName_;
    pairingInFlight_.insert(conn->id());
    emit pairingInFlightChanged();
    auto* watcher = new QFutureWatcher<PairingClient::Reply>(this);
    QObject::connect(
        watcher, &QFutureWatcherBase::finished, this, [this, watcher, conn, server, intent] {
            const auto pair = watcher->result();
            watcher->deleteLater();
            pairingInFlight_.remove(conn->id());
            emit pairingInFlightChanged();
            const auto outcome = PairingClient::classify(pair.response, pair.pinMismatch);
            std::visit(
                [&](auto&& arm) {
                    using T = std::decay_t<decltype(arm)>;
                    if constexpr (std::is_same_v<T, PairingClient::Success>) {
                        store_->setSharedKey(arm.sharedKeyHex, WifiConnection::idFor(server));
                        openSession(conn, server, intent);
                    } else if constexpr (std::is_same_v<T, PairingClient::AuthRequired> ||
                                         std::is_same_v<T, PairingClient::Pending>) {
                        // First-time pair, or the server forgot us. Silent intents
                        // park in Stale so the next user tap gets the dialog.
                        if (intent == ConnectIntent::UserInitiated) {
                            conn->markDisconnected();
                            emit connectionEvent(pairingRequired(server));
                        } else {
                            conn->markStale();
                        }
                    } else if constexpr (std::is_same_v<T, PairingClient::VersionMismatch>) {
                        conn->markDisconnected();
                        emitErrorIfUserInitiated(intent, versionMsg());
                    } else if constexpr (std::is_same_v<T, PairingClient::IdentityChanged>) {
                        conn->markDisconnected();
                        emitErrorIfUserInitiated(intent, identityChangedMsg());
                    } else {
                        if (intent == ConnectIntent::UserInitiated) {
                            conn->markDisconnected();
                            emit connectionEvent(makeError(unreachableMsg()));
                        } else {
                            conn->markStale();
                        }
                    }
                },
                outcome);
        });
    watcher->setFuture(QtConcurrent::run([server, did, dname] {
        return PairingClient::pair(server.ip, server.pairPort, did, dname, QString());
    }));
}

void WifiConnectionManager::openSession(WifiConnection* conn,
                                        const models::DiscoveredServer& server,
                                        ConnectIntent intent) {
    const QString id = conn->id();
    auto creds = credentialsFor(id);
    if (!creds.has_value()) {
        onTerminalAuthFailure(conn, id, intent);
        return;
    }
    const QString proof = creds->proof;
    const auto descriptors = conn->desiredDescriptors();
    const bool wantsMouse = conn->wantsMouseControl();
    const auto pairingKey = creds->pairingKey;
    // Lets the response callback converge slot changes that raced the round-trip.
    // NOT const: a const capture is copied rather than moved into the
    // std::function, and that copy can throw out of the closure's move ctor.
    auto sentDescriptors = descriptorsToDesired(descriptors);

    http_->putSession(
        server.ip, server.httpPort, deviceId_, deviceName_, proof, descriptors, wantsMouse,
        [this, conn, server, intent, id, pairingKey,
         sentDescriptors](const models::SessionResponse& resp, bool pinMismatch) {
            using reducer::RestVerdict;
            reducer::RestReply rr;
            rr.status = resp.httpStatus;
            rr.bodyParsed = resp.reachable;
            rr.code = resp.code.value_or(QString()).toStdString();
            rr.pinMismatch = pinMismatch;
            const RestVerdict verdict = classifyRest(rr);
            if (verdict == RestVerdict::Unauthorized) {
                onTerminalAuthFailure(conn, id, intent);
                return;
            }
            if (verdict == RestVerdict::VersionMismatch) {
                conn->markDisconnected();
                emitErrorIfUserInitiated(intent, versionMsg());
                return;
            }
            if (verdict == RestVerdict::IdentityChanged) {
                // The pin still guards the OLD cert, so no retry can succeed: it
                // takes a Forget to drop it. Falling through would park this on the
                // backoff curve as if the box were merely offline.
                conn->markDisconnected();
                retryAttempts_.remove(id);
                emitErrorIfUserInitiated(intent, identityChangedMsg());
                return;
            }
            if (verdict != RestVerdict::Ok || !resp.connectionId || !resp.token ||
                !resp.sessionSalt) {
                // Unreachable, 503 or malformed: park and back off.
                if (intent == ConnectIntent::UserInitiated) {
                    conn->markDisconnected();
                    emit connectionEvent(makeError(unreachableMsg()));
                } else {
                    conn->markStale();
                }
                scheduleRetry(server, intent);
                return;
            }
            // Malformed material degrades like a refused connect, never a crash.
            const auto tok = util::fromHex(resp.token->toStdString());
            const auto salt = util::fromHex(resp.sessionSalt->toStdString());
            if (!tok || tok->size() != 4 || !salt || salt->size() != 8) {
                conn->markDisconnected();
                return;
            }
            std::array<std::uint8_t, 4> token{};
            std::copy_n(tok->begin(), 4, token.begin());
            std::array<std::uint8_t, 8> saltArr{};
            std::copy_n(salt->begin(), 8, saltArr.begin());
            // The pairing key itself never reaches the UDP path; only this
            // derived per-session key does.
            const std::uint32_t tokenBe = (static_cast<std::uint32_t>(token[0]) << 24) |
                                          (static_cast<std::uint32_t>(token[1]) << 16) |
                                          (static_cast<std::uint32_t>(token[2]) << 8) |
                                          static_cast<std::uint32_t>(token[3]);
            std::array<std::uint8_t, 32> sessionKey{};
            wire::deriveSessionKey(pairingKey.data(), saltArr.data(), tokenBe, sessionKey.data());

            auto client = std::make_shared<SatelliteClient>();
            if (!client->openSocket(server.ip.toStdString(), server.udpPort)) {
                conn->markDisconnected();
                return;
            }
            client->setConnectionParams(token, sessionKey);
            store_->remember(server);
            retryAttempts_.remove(id);

            conn->markConnected(
                client, *resp.connectionId, resp.epoch, resp.mouseControl.granted,
                /*onDead=*/
                [this, id, server] {
                    disconnect(id);
                    scheduleRetry(server, ConnectIntent::RetryAfterDeath);
                },
                /*onClose=*/
                [this, id, server](std::uint8_t reason) {
                    if (auto* c = connections_.value(id, nullptr)) {
                        handleServerClose(c, server, reason);
                    }
                },
                /*onReconcile=*/
                [this, id, server] {
                    if (auto* c = connections_.value(id, nullptr)) { reconcile(c, server); }
                },
                /*onRekey=*/
                [this, id, server] {
                    if (auto* c = connections_.value(id, nullptr)) { rekey(c, server); }
                });
            conn->applyResults(resp.controllers);
            const auto converge = reducer::lateSlotConverge(
                sentDescriptors, descriptorsToDesired(conn->desiredDescriptors()));
            for (std::uint8_t ctrlIdx : converge.removes) { deleteSlot(id, ctrlIdx); }
            for (std::uint8_t ctrlIdx : converge.resyncs) {
                const QString slotId = conn->slotIdForIndex(ctrlIdx);
                if (!slotId.isEmpty()) { syncSlot(id, slotId); }
            }
        });
}

void WifiConnectionManager::reconcile(WifiConnection* conn,
                                      const models::DiscoveredServer& server) {
    const QString id = conn->id();
    if (conn->state() != SessionState::Live) { return; }
    const auto connId = conn->connectionId();
    if (!connId.has_value()) { return; }
    auto client = conn->client();
    if (!client) { return; }
    if (!reducer::reconcileNeeded(client->serverEpoch(), client->serverBitmap(),
                                  conn->lastAppliedEpoch(), conn->registeredBitmap())) {
        return;
    }
    if (reconcileInFlight_.contains(id)) { return; }
    reconcileInFlight_.insert(id);
    const auto creds = credentialsFor(id);
    if (!creds.has_value()) {
        reconcileInFlight_.remove(id);
        return;
    }
    http_->getSession(server.ip, server.httpPort, *connId, deviceId_, creds->proof,
                      [this, id, server](const models::SessionViewDto& view) {
                          reconcileInFlight_.remove(id);
                          auto* c = connections_.value(id, nullptr);
                          if (c == nullptr || c->state() != SessionState::Live) { return; }
                          if (view.unauthorized()) {
                              onTerminalAuthFailure(c, id, ConnectIntent::RetryAfterDeath);
                              return;
                          }
                          if (!view.reachable || view.httpStatus < 200 || view.httpStatus > 299) {
                              return;
                          }
                          if (view.connectionId == c->connectionId() &&
                              c->matchesAppliedView(view)) {
                              // Benign drift, e.g. our own standalone PUT raced an ack.
                              c->adoptEpoch(view.epoch);
                              return;
                          }
                          // Tear the UDP tuple down first, since the converging PUT
                          // rotates the token and key.
                          c->markDisconnected();
                          c->markConnecting();
                          openSession(c, server, ConnectIntent::RetryAfterDeath);
                      });
}

void WifiConnectionManager::rekey(WifiConnection* conn, const models::DiscoveredServer& server) {
    const QString id = conn->id();
    if (conn->state() != SessionState::Live) { return; }
    const auto client = conn->client();
    if (!client) { return; }
    const auto creds = credentialsFor(id);
    if (!creds.has_value()) { return; }
    const auto pairingKey = creds->pairingKey;
    // Failures stay silent: heartbeat death and terminal-auth already surface
    // them, and a session that truly exhausts self-heals via the death retry.
    http_->putSession(
        server.ip, server.httpPort, deviceId_, deviceName_, creds->proof,
        conn->desiredDescriptors(), conn->wantsMouseControl(),
        [this, id, client, pairingKey](const models::SessionResponse& resp, bool pinMismatch) {
            auto* c = connections_.value(id, nullptr);
            if (c == nullptr) { return; }
            using reducer::RestVerdict;
            reducer::RestReply rr;
            rr.status = resp.httpStatus;
            rr.bodyParsed = resp.reachable;
            rr.code = resp.code.value_or(QString()).toStdString();
            rr.pinMismatch = pinMismatch;
            const RestVerdict verdict = classifyRest(rr);
            if (verdict == RestVerdict::Unauthorized) {
                onTerminalAuthFailure(c, id, ConnectIntent::RetryAfterDeath);
                return;
            }
            // If a death and reconnect replaced the session mid-flight, applying
            // this material would re-arm the dead client and stamp a stale epoch
            // onto the new session.
            if (c->state() != SessionState::Live || c->client() != client) { return; }
            if (verdict != RestVerdict::Ok || !resp.token.has_value() ||
                !resp.sessionSalt.has_value()) {
                return;
            }
            const auto tok = util::fromHex(resp.token->toStdString());
            const auto salt = util::fromHex(resp.sessionSalt->toStdString());
            if (!tok || tok->size() != 4 || !salt || salt->size() != wire::kSessionSaltSize) {
                return;
            }
            std::array<std::uint8_t, 4> token{};
            std::copy_n(tok->begin(), 4, token.begin());
            std::array<std::uint8_t, wire::kSessionSaltSize> saltArr{};
            std::copy_n(salt->begin(), wire::kSessionSaltSize, saltArr.begin());
            const std::uint32_t tokenBe = (static_cast<std::uint32_t>(token[0]) << 24) |
                                          (static_cast<std::uint32_t>(token[1]) << 16) |
                                          (static_cast<std::uint32_t>(token[2]) << 8) |
                                          static_cast<std::uint32_t>(token[3]);
            std::array<std::uint8_t, 32> sessionKey{};
            wire::deriveSessionKey(pairingKey.data(), saltArr.data(), tokenBe, sessionKey.data());
            // Same socket, fresh token and key, counters back to 1, so the hot
            // path never blips. connectionId is stable across PUTs, so the id and
            // slot state carry over.
            client->setConnectionParams(token, sessionKey);
            // Otherwise the next enriched ack would read as drift.
            c->adoptEpoch(resp.epoch);
        });
}

void WifiConnectionManager::syncSlot(const QString& id, const QString& slotId) {
    auto* conn = connections_.value(id, nullptr);
    if (conn == nullptr || conn->state() != SessionState::Live) { return; }
    const auto connId = conn->connectionId();
    if (!connId.has_value()) { return; }
    const auto descriptor = conn->descriptorFor(slotId);
    if (!descriptor.has_value()) { return; }
    const auto creds = credentialsFor(id);
    if (!creds.has_value()) { return; }
    const models::DiscoveredServer server = conn->server();
    http_->putController(server.ip, server.httpPort, *connId, deviceId_, creds->proof, *descriptor,
                         [this, id, server](const models::ControllerPutResponse& resp) {
                             auto* c = connections_.value(id, nullptr);
                             if (c == nullptr) { return; }
                             if (resp.unauthorized()) {
                                 onTerminalAuthFailure(c, id, ConnectIntent::RetryAfterDeath);
                                 return;
                             }
                             if (!resp.controller.has_value()) {
                                 // 404: the session died under us, and the alive-poll
                                 // and close-notify paths own recovery.
                                 return;
                             }
                             c->adoptEpoch(resp.epoch);
                             c->applyResults(QList<models::ControllerApplyDto>{*resp.controller});
                             // The grant is only computed at session PUT, so a
                             // mouse-mode toggle needs the whole session converged.
                             if (c->wantsMouseControl() != c->mouseControlGranted()) {
                                 reconcile(c, server);
                             }
                         });
}

void WifiConnectionManager::deleteSlot(const QString& id, int ctrlIdx) {
    auto* conn = connections_.value(id, nullptr);
    if (conn == nullptr) { return; }
    const auto connId = conn->connectionId();
    if (!connId.has_value()) { return; }
    const auto creds = credentialsFor(id);
    if (!creds.has_value()) { return; }
    const models::DiscoveredServer server = conn->server();
    http_->deleteController(server.ip, server.httpPort, *connId, ctrlIdx, deviceId_, creds->proof,
                            [this, id](const models::ControllerPutResponse& resp) {
                                auto* c = connections_.value(id, nullptr);
                                if (c == nullptr) { return; }
                                if (!resp.error.has_value()) { c->adoptEpoch(resp.epoch); }
                            });
}

void WifiConnectionManager::handleServerClose(WifiConnection* conn,
                                              const models::DiscoveredServer& server,
                                              std::uint8_t reason) {
    const QString id = conn->id();
    switch (reducer::closeActionForReason(reason)) {
    case reducer::CloseAction::DropKeyRePair:
        // unpaired: trust revoked.
        conn->markStale();
        store_->forgetKey(id);
        break;
    case reducer::CloseAction::StayDown:
        // replaced: a newer PUT already owns the session.
        conn->markDisconnected();
        break;
    case reducer::CloseAction::RetryBackoff:
        // shutdown or kicked, both transient.
        conn->markDisconnected();
        scheduleRetry(server, ConnectIntent::RetryAfterDeath);
        break;
    }
}

void WifiConnectionManager::scheduleRetry(const models::DiscoveredServer& server,
                                          ConnectIntent intent) {
    if (intent == ConnectIntent::UserInitiated) { return; }
    const QString id = server.id();
    const int attempt = retryAttempts_.value(id, 0) + 1;
    retryAttempts_.insert(id, attempt);
    const auto delay = reducer::backoffDelayMs(attempt);
    QTimer::singleShot(static_cast<int>(delay), this, [this, id, server] {
        auto* c = connections_.value(id, nullptr);
        if (c == nullptr) { return; }
        // Retry only from a settled state: a user-driven reconnect or forget in
        // the interim moved it out, and clobbering that would fight the user.
        if (c->state() != SessionState::Idle && c->state() != SessionState::Stale) { return; }
        // Idempotent, and on completion it persists any new IP and re-runs
        // autoReconnectAll, so a box that moved DHCP leases reconnects on its own
        // without the user opening Manage and pressing Scan.
        startDiscovery();
        // The direct attempt below still runs, so a satellite discovery cannot
        // reach (mDNS and broadcast blocked on the segment) is not left waiting on
        // a scan that may find nothing.
        models::DiscoveredServer target = server;
        for (const auto& r : store_->remembered()) {
            if (r.id == id) {
                target = r.toDiscovered();
                break;
            }
        }
        connectTo(target, ConnectIntent::RetryAfterDeath);
    });
}

void WifiConnectionManager::onTerminalAuthFailure(WifiConnection* conn, const QString& id,
                                                  ConnectIntent intent) {
    // 401 NOT_PAIRED / BAD_PROOF, or no usable key at all. Terminal by contract,
    // so the retry curve stops here rather than hammering a revoked device.
    conn->markStale();
    store_->forgetKey(id);
    retryAttempts_.remove(id);
    emitErrorIfUserInitiated(intent, rePairMsg());
}

void WifiConnectionManager::emitErrorIfUserInitiated(ConnectIntent intent, const QString& message) {
    if (intent == ConnectIntent::UserInitiated) { emit connectionEvent(makeError(message)); }
}

void WifiConnectionManager::markStale(const QString& id) {
    if (auto* conn = connections_.value(id, nullptr)) { conn->markStale(); }
}

void WifiConnectionManager::disconnect(const QString& id) {
    auto* conn = connections_.value(id, nullptr);
    if (conn == nullptr) { return; }
    const auto server = conn->server();
    const auto cid = conn->connectionId();
    const auto creds = credentialsFor(id);
    conn->markDisconnected();
    // Best-effort only: the local side already treats the session as gone.
    if (cid.has_value() && creds.has_value()) {
        http_->deleteSession(server.ip, server.httpPort, *cid, deviceId_, creds->proof,
                             [](int, bool, const QString&) {});
    }
}

void WifiConnectionManager::forget(const QString& id) {
    auto* conn = connections_.value(id, nullptr);
    // Self-unpair BEFORE dropping the key, since the proof needs it. Otherwise a
    // forgotten dish leaves a paired ghost row on the satellite.
    if (conn != nullptr) {
        const auto server = conn->server();
        const auto creds = credentialsFor(id);
        if (creds.has_value()) {
            http_->unpair(server.ip, server.httpPort, deviceId_, creds->proof,
                          [](int, bool, const QString&) {});
        }
    }
    disconnect(id);
    store_->forget(id);
    retryAttempts_.remove(id);
    reconcileInFlight_.remove(id);
    if (auto* taken = connections_.take(id)) {
        taken->deleteLater();
        emit poolChanged();
    }
}

void WifiConnectionManager::prepareForSleep() {
    // Snapshot the keys: disconnect() fans out through poolChanged into the
    // hub's rebuild, which reshapes connections_ under a live iterator.
    const auto ids = connections_.keys();
    for (const auto& id : ids) { disconnect(id); }
}

void WifiConnectionManager::resumeFromSleep() {
    // A backoff curve armed before the suspend is measuring wall clock the
    // machine spent asleep, so the first attempt after a resume starts over.
    retryAttempts_.clear();
    // Rescan before reconnecting: a laptop that resumes on another network has
    // a stale remembered IP, and only discovery can relearn it.
    startDiscovery();
    autoReconnectAll();
}

void WifiConnectionManager::autoReconnectAll() {
    for (const auto& r : store_->remembered()) {
        auto* existing = connections_.value(r.id, nullptr);
        if (existing == nullptr || existing->state() != SessionState::Live) {
            connectTo(r.toDiscovered(), ConnectIntent::AutoReconnect);
        }
    }
}

} // namespace dish::net
