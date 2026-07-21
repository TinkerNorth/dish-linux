// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "WifiConnectionManager.h"

#include "Backoff.h"
#include "CloseNotify.h"
#include "LANDiscovery.h"
#include "MdnsDiscovery.h"
#include "PairingClient.h"
#include "Reconcile.h"
#include "SessionCrypto.h"
#include "Tofu.h"
#include "Util/Hex.h"

#include <QDateTime>
#include <QHostInfo>
#include <QtConcurrent/QtConcurrent>
#include <QtGlobal>

#include <type_traits>
#include <variant>
#include <vector>

namespace dish::net {

namespace {

ConnectionEvent makeError(const QString& msg) { return {ConnectionEventKind::Error, {}, msg}; }

ConnectionEvent pairingRequired(const models::DiscoveredServer& s) {
    return {ConnectionEventKind::PairingRequired, s, {}};
}

// Host u32 view of the 4 raw token bytes (big-endian order), the form
// wire::deriveSessionKey folds into the HKDF info block.
std::uint32_t tokenToU32(const std::array<std::uint8_t, 4>& t) {
    return (static_cast<std::uint32_t>(t[0]) << 24) | (static_cast<std::uint32_t>(t[1]) << 16) |
           (static_cast<std::uint32_t>(t[2]) << 8) | static_cast<std::uint32_t>(t[3]);
}

} // namespace

WifiConnectionManager::WifiConnectionManager(ConnectionStore* store, QObject* parent)
    : QObject(parent), store_(store), http_(new HTTPClient(this)) {
    deviceId_ = store_->getOrCreateDeviceId();
    deviceName_ = QHostInfo::localHostName();
    if (deviceName_.isEmpty()) { deviceName_ = QStringLiteral("Linux"); }
    http_->setPinVerifier(makePinVerifier());
}

WifiConnectionManager::~WifiConnectionManager() {
    for (auto* c : connections_) { c->markDisconnected(); }
}

HTTPClient::PinVerifier WifiConnectionManager::makePinVerifier() const {
    // TOFU: first contact pins the cert's SHA-256 fingerprint for this host;
    // any later cert that differs is rejected (anti-MITM). The store's pin
    // accessors are mutex-guarded because PairingClient shares this verifier
    // from a QtConcurrent worker thread.
    return [store = store_](const QString& host, const QByteArray& der) {
        const std::string presented =
            sha256FingerprintHex(reinterpret_cast<const std::uint8_t*>(der.constData()),
                                 static_cast<std::size_t>(der.size()));
        const auto storedPin = store->certPin(host);
        std::optional<std::string> stored;
        if (storedPin.has_value()) { stored = storedPin->toStdString(); }
        switch (tofuVerdict(stored, presented)) {
        case TofuVerdict::TrustFirstUse:
            store->setCertPin(host, QString::fromStdString(presented));
            return true;
        case TofuVerdict::Match:
            return true;
        case TofuVerdict::Mismatch:
            return false;
        }
        return false;
    };
}

std::optional<std::array<std::uint8_t, 32>>
WifiConnectionManager::pairingKeyFor(const QString& id) const {
    const auto keyHex = store_->sharedKey(id);
    if (!keyHex.has_value() || keyHex->size() != 64) { return std::nullopt; }
    const auto keyBytes = util::fromHex(keyHex->toStdString());
    if (!keyBytes || keyBytes->size() != 32) { return std::nullopt; }
    std::array<std::uint8_t, 32> key{};
    std::copy_n(keyBytes->begin(), 32, key.begin());
    return key;
}

QString WifiConnectionManager::proofFor(const QString& id) const {
    const auto key = pairingKeyFor(id);
    if (!key.has_value()) { return {}; }
    return QString::fromStdString(wire::computeHmacProof(key->data(), deviceId_.toStdString()));
}

void WifiConnectionManager::startDiscovery() {
    if (scanning_) { return; }
    scanning_ = true;
    emit scanningChanged();
    auto* watcher = new QFutureWatcher<QList<models::DiscoveredServer>>(this);
    QObject::connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher] {
        discovered_ = watcher->result();
        scanning_ = false;
        // Re-home remembered satellites whose machineId matched under a new
        // address (DHCP move): the store refreshes rows in place, and any
        // idle pool entry re-points so the next connect targets the current
        // endpoint.
        store_->refreshFromDiscovery(discovered_);
        for (const auto& d : discovered_) {
            if (auto* conn = connections_.value(d.id(), nullptr)) {
                if (conn->state() == SessionState::Idle) { conn->updateServer(d); }
            }
        }
        emit discoveredChanged();
        emit scanningChanged();
        if (discovered_.isEmpty()) {
            emit connectionEvent(
                makeError(QStringLiteral("No servers found — check your network")));
        }
        watcher->deleteLater();
    });
    // Two discovery paths in parallel, merged by stable id: the legacy UDP
    // broadcast beacon and mDNS / Bonjour. mDNS reaches subnets that drop
    // broadcast; the beacon stays as the fallback for pre-responder
    // satellites. The mDNS scan runs on a second pool thread so the combined
    // wall time is one timeout, not two.
    watcher->setFuture(QtConcurrent::run([] {
        auto mdnsFuture = QtConcurrent::run([] { return MdnsDiscovery::discover(); });
        const QList<models::DiscoveredServer> beacon = LANDiscovery::discover();
        const QList<models::DiscoveredServer> mdns = mdnsFuture.result();
        const QList<models::DiscoveredServer> merged = mergeDiscovered(beacon, mdns);
        // Per-path discovery logging so the broadcast vs mDNS hit-rate can be
        // compared in the field (Task 1.6).
        qInfo("discovery scan: broadcast=%lld mdns=%lld merged=%lld",
              static_cast<long long>(beacon.size()), static_cast<long long>(mdns.size()),
              static_cast<long long>(merged.size()));
        return merged;
    }));
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
    QObject::connect(conn, &WifiConnection::registrationFailed, this,
                     &WifiConnectionManager::slotRegistrationFailed);
    emit poolChanged();
    return conn;
}

void WifiConnectionManager::connectTo(const models::DiscoveredServer& server,
                                      ConnectIntent intent) {
    auto* conn = ensureConnection(server);
    if (intent == ConnectIntent::UserInitiated) { clearRetry(conn->id()); }
    if (conn->state() == SessionState::Live || conn->state() == SessionState::Faltering ||
        conn->state() == SessionState::Linking) {
        conn->updateServer(server);
        return;
    }
    conn->updateServer(server);
    conn->markConnecting();
    pairAndConnect(conn, server, QString(), intent);
}

void WifiConnectionManager::pairWithPin(const models::DiscoveredServer& server,
                                        const QString& pin) {
    auto* conn = ensureConnection(server);
    clearRetry(conn->id());
    conn->markConnecting();
    pairAndConnect(conn, server, pin, ConnectIntent::UserInitiated);
}

void WifiConnectionManager::pairAndConnect(WifiConnection* conn,
                                           const models::DiscoveredServer& server,
                                           const QString& pin, ConnectIntent intent) {
    // Auto-reconnect fast path (pin.isEmpty()): if we already have a shared
    // key saved for this server, skip the pair handshake entirely and go
    // straight to the session PUT. A moved/offline server then fails fast in
    // the HTTP layer instead of bouncing through pair → PairingRequired and
    // trapping the user behind a PIN prompt that can't be satisfied.
    if (pin.isEmpty()) {
        if (pairingKeyFor(WifiConnection::idFor(server)).has_value()) {
            openSession(conn, server, intent);
            return;
        }
        // No key + non-user intent: nothing to retry against — auto paths
        // never spam the PIN prompt.
        if (intent != ConnectIntent::UserInitiated) {
            conn->markDisconnected();
            return;
        }
    }
    const QString did = deviceId_;
    const QString dname = deviceName_;
    const auto verifier = makePinVerifier();
    auto* watcher = new QFutureWatcher<models::PairResponse>(this);
    QObject::connect(
        watcher, &QFutureWatcherBase::finished, this, [this, watcher, conn, server, pin, intent] {
            const auto pair = watcher->result();
            watcher->deleteLater();
            const auto outcome = PairingClient::classify(pair);
            std::visit(
                [&](auto&& arm) {
                    using T = std::decay_t<decltype(arm)>;
                    if constexpr (std::is_same_v<T, PairingClient::Success>) {
                        store_->setSharedKey(arm.sharedKeyHex, WifiConnection::idFor(server));
                        openSession(conn, server, intent);
                    } else if constexpr (std::is_same_v<T, PairingClient::AuthRequired>) {
                        conn->markDisconnected();
                        if (pin.isEmpty()) {
                            emit connectionEvent(pairingRequired(server));
                        } else {
                            emit connectionEvent(
                                makeError(pair.error.value_or(QStringLiteral("Pairing failed"))));
                        }
                    } else if constexpr (std::is_same_v<T, PairingClient::Unreachable>) {
                        conn->markDisconnected();
                        if (intent == ConnectIntent::UserInitiated) {
                            emit connectionEvent(makeError(
                                QStringLiteral("Server unreachable — has it moved networks? (%1)")
                                    .arg(arm.message)));
                        } else {
                            scheduleRetry(conn->id());
                        }
                    }
                },
                outcome);
        });
    watcher->setFuture(QtConcurrent::run([server, did, dname, pin, verifier] {
        return PairingClient::pair(server.ip, server.pairPort, did, dname, pin, verifier);
    }));
}

void WifiConnectionManager::openSession(WifiConnection* conn,
                                        const models::DiscoveredServer& server,
                                        ConnectIntent intent) {
    const auto id = WifiConnection::idFor(server);
    const auto pairingKey = pairingKeyFor(id);
    if (!pairingKey.has_value()) {
        conn->markDisconnected();
        if (intent == ConnectIntent::UserInitiated) {
            emit connectionEvent(makeError(QStringLiteral("No shared key — re-pair needed")));
        }
        return;
    }
    const QString proof =
        QString::fromStdString(wire::computeHmacProof(pairingKey->data(), deviceId_.toStdString()));

    // The declarative PUT carries the WHOLE desired controller set (empty is a
    // valid zero-controller session). Mouse-mode is deferred: no UI requests
    // it yet, but the grant still parses.
    QList<models::ControllerDescriptor> descriptors;
    if (const auto desc = conn->desiredDescriptor(); desc.has_value()) {
        descriptors.append(*desc);
    }

    http_->putSession(
        server.ip, server.httpPort, deviceId_, deviceName_, proof, descriptors, false,
        [this, conn, server, id, intent, pairingKey,
         hadDescriptor = !descriptors.isEmpty()](const models::SessionResponse& resp) {
            if (resp.unauthorized()) {
                handleTerminalAuth(id, intent == ConnectIntent::UserInitiated);
                return;
            }
            if (!resp.reachable || !resp.connectionId.has_value() || !resp.token.has_value() ||
                !resp.sessionSalt.has_value()) {
                conn->markDisconnected();
                if (intent == ConnectIntent::UserInitiated) {
                    emit connectionEvent(makeError(
                        QStringLiteral("Error: %1")
                            .arg(resp.error.value_or(QStringLiteral("connection failed")))));
                } else {
                    scheduleRetry(id);
                }
                return;
            }
            const auto tok = util::fromHex(resp.token->toStdString());
            const auto salt = util::fromHex(resp.sessionSalt->toStdString());
            if (!tok || tok->size() != 4 || !salt || salt->size() != wire::kSessionSaltSize) {
                conn->markDisconnected();
                if (intent == ConnectIntent::UserInitiated) {
                    emit connectionEvent(makeError(QStringLiteral("Bad token from server")));
                }
                return;
            }
            std::array<std::uint8_t, 4> token{};
            std::copy_n(tok->begin(), 4, token.begin());
            std::array<std::uint8_t, wire::kSessionSaltSize> saltArr{};
            std::copy_n(salt->begin(), wire::kSessionSaltSize, saltArr.begin());

            // sessionKey = HKDF(pairingKey, salt, token) — the pairing key
            // itself never touches the UDP path (contract §Crypto).
            std::array<std::uint8_t, 32> sessionKey{};
            wire::deriveSessionKey(pairingKey->data(), saltArr.data(), tokenToU32(token),
                                   sessionKey.data());

            auto client = std::make_shared<SatelliteClient>();
            if (!client->openSocket(server.ip.toStdString(), server.udpPort)) {
                conn->markDisconnected();
                if (intent != ConnectIntent::UserInitiated) { scheduleRetry(id); }
                return;
            }
            client->setConnectionParams(token, sessionKey);
            store_->remember(server);
            clearRetry(id);

            // Surface the bound slot's apply outcome from the PUT itself.
            bool slotLive = false;
            for (const auto& applied : resp.controllers) {
                if (applied.ctrlIdx == 0) { slotLive = applied.slotIsLive(); }
            }
            const QString cid = *resp.connectionId;
            conn->markConnected(client, cid, resp.epoch, makeHooks(conn->id()));
            if (hadDescriptor) {
                if (slotLive) {
                    conn->markSlotApplied();
                } else if (const auto slot = conn->boundSlotId(); slot.has_value()) {
                    emit connectionEvent(
                        makeError(QStringLiteral("Server could not apply the controller")));
                    emit slotRegistrationFailed(*slot);
                }
            }
        });
}

WifiConnection::SessionHooks WifiConnectionManager::makeHooks(const QString& id) {
    WifiConnection::SessionHooks hooks;
    hooks.onDead = [this, id] { handleDead(id); };
    hooks.onClose = [this, id](std::uint8_t reason) { handleClose(id, reason); };
    hooks.putSlot = [this, id](const models::ControllerDescriptor& desc,
                               std::function<void(const models::ControllerPutResponse&)> cb) {
        auto* conn = connections_.value(id, nullptr);
        if (conn == nullptr || !conn->connectionId().has_value()) { return; }
        const auto server = conn->server();
        http_->putController(
            server.ip, server.httpPort, *conn->connectionId(), deviceId_, proofFor(id), desc,
            [this, id, cb = std::move(cb)](const models::ControllerPutResponse& resp) {
                if (resp.unauthorized()) { handleTerminalAuth(id, true); }
                cb(resp);
            });
    };
    hooks.deleteSlot = [this, id](int ctrlIdx) {
        auto* conn = connections_.value(id, nullptr);
        if (conn == nullptr || !conn->connectionId().has_value()) { return; }
        const auto server = conn->server();
        http_->deleteController(server.ip, server.httpPort, *conn->connectionId(), ctrlIdx,
                                deviceId_, proofFor(id),
                                [](const models::ControllerPutResponse&) {});
    };
    hooks.reconcile = [this, id] { runReconcile(id); };
    hooks.rekey = [this, id] { runRekey(id); };
    return hooks;
}

void WifiConnectionManager::handleTerminalAuth(const QString& id, bool loud) {
    // NOT_PAIRED / BAD_PROOF: the satellite revoked our trust. Drop ONLY the
    // key — the remembered row survives so the UI parks it on "Needs pairing"
    // (LinkState::Stale) instead of silently deleting the satellite.
    store_->forgetKey(id);
    clearRetry(id);
    if (auto* conn = connections_.value(id, nullptr)) { conn->markDisconnected(); }
    if (loud) {
        emit connectionEvent(
            makeError(QStringLiteral("The satellite no longer trusts this device — pair again")));
    }
    emit poolChanged();
}

void WifiConnectionManager::handleDead(const QString& id) {
    auto* conn = connections_.value(id, nullptr);
    if (conn == nullptr) { return; }
    conn->markDisconnected();
    // Silent: death retries ride the backoff curve, the UI just shows the
    // state flip. autoReconnectAll picks the retry up when it comes due.
    scheduleRetry(id);
}

void WifiConnectionManager::handleClose(const QString& id, std::uint8_t reason) {
    auto* conn = connections_.value(id, nullptr);
    if (conn == nullptr) { return; }
    switch (reducer::closeActionForReason(reason)) {
    case reducer::CloseAction::DropKeyRePair:
        handleTerminalAuth(id, true);
        return;
    case reducer::CloseAction::StayDown:
        // A newer session (this device or another) owns the satellite;
        // auto-reconnecting would kick it. Park until the user acts.
        conn->markDisconnected();
        retry_[id] = RetryState{0, 0, /*suppressed=*/true};
        return;
    case reducer::CloseAction::RetryBackoff:
        conn->markDisconnected();
        scheduleRetry(id);
        return;
    }
}

void WifiConnectionManager::runReconcile(const QString& id) {
    auto* conn = connections_.value(id, nullptr);
    if (conn == nullptr) { return; }
    const auto connectionId = conn->connectionId();
    if (!connectionId.has_value()) { return; }
    conn->setReconcileInFlight(true);
    const auto server = conn->server();
    http_->getSession(
        server.ip, server.httpPort, *connectionId, deviceId_, proofFor(id),
        [this, id](const models::SessionViewDto& view) {
            auto* c = connections_.value(id, nullptr);
            if (c == nullptr) { return; }
            c->setReconcileInFlight(false);
            if (view.unauthorized()) {
                handleTerminalAuth(id, false);
                return;
            }
            if (!view.reachable) { return; } // transient — the next drift tick retries
            std::vector<reducer::DesiredSlot> desired;
            if (const auto desc = c->desiredDescriptor(); desc.has_value() && c->boundSlotId()) {
                desired.push_back({static_cast<std::uint8_t>(desc->ctrlIdx), desc->type});
            }
            std::vector<reducer::AppliedSlot> applied;
            for (const auto& a : view.controllers) {
                applied.push_back({static_cast<std::uint8_t>(a.ctrlIdx),
                                   static_cast<std::uint8_t>(a.appliedType), a.active});
            }
            if (reducer::appliedMatchesDesired(desired, applied)) {
                // Benign drift (e.g. the server bumped the epoch converging
                // our own PUT): adopt the epoch, keep the session.
                c->setLastAppliedEpoch(view.epoch);
                return;
            }
            // Real divergence: re-PUT the full desired topology. The
            // declarative PUT replaces the session (token rotates), so this
            // rides the normal open path; the brief Connecting flip is honest.
            c->markDisconnected();
            c->markConnecting();
            openSession(c, c->server(), ConnectIntent::RetryAfterDeath);
        });
}

void WifiConnectionManager::runRekey(const QString& id) {
    auto* conn = connections_.value(id, nullptr);
    if (conn == nullptr) { return; }
    // Faltering still counts as live: REST can be healthy while UDP acks are
    // lossy, and bailing after the server rotated the token would orphan the
    // session.
    if (conn->state() != SessionState::Live && conn->state() != SessionState::Faltering) { return; }
    const auto client = conn->client();
    if (!client) { return; }
    const auto pairingKey = pairingKeyFor(id);
    if (!pairingKey.has_value()) { return; }
    const QString proof =
        QString::fromStdString(wire::computeHmacProof(pairingKey->data(), deviceId_.toStdString()));
    QList<models::ControllerDescriptor> descriptors;
    if (const auto desc = conn->desiredDescriptor(); desc.has_value()) {
        descriptors.append(*desc);
    }
    const auto server = conn->server();
    // Failures stay silent: heartbeat death / terminal-auth already surface
    // them, and a session that truly exhausts goes silent and self-heals via
    // the death-retry re-PUT.
    http_->putSession(
        server.ip, server.httpPort, deviceId_, deviceName_, proof, descriptors, false,
        [this, id, client, pairingKey](const models::SessionResponse& resp) {
            if (resp.unauthorized()) {
                handleTerminalAuth(id, false);
                return;
            }
            auto* c = connections_.value(id, nullptr);
            if (c == nullptr) { return; }
            // A death+reconnect during the PUT flight replaced the session —
            // applying the stale material would re-arm the dead client and
            // stamp a stale epoch onto the new session.
            if (c->state() != SessionState::Live && c->state() != SessionState::Faltering) {
                return;
            }
            if (c->client() != client) { return; }
            if (!resp.reachable || !resp.token.has_value() || !resp.sessionSalt.has_value()) {
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
            std::array<std::uint8_t, 32> sessionKey{};
            wire::deriveSessionKey(pairingKey->data(), saltArr.data(), tokenToU32(token),
                                   sessionKey.data());
            // Same socket, fresh token/key, counters restart at 1 — the hot
            // path never blips. connectionId is stable across PUTs (contract
            // §Session), so the id and slot state carry over.
            client->setConnectionParams(token, sessionKey);
            // Adopt the re-PUT's epoch so the next enriched ack doesn't read
            // as drift.
            c->setLastAppliedEpoch(resp.epoch);
        });
}

void WifiConnectionManager::scheduleRetry(const QString& id) {
    auto& state = retry_[id];
    if (state.suppressed) { return; }
    state.attempt += 1;
    state.nextRetryAtMs =
        QDateTime::currentMSecsSinceEpoch() + reducer::backoffDelayMs(state.attempt);
}

void WifiConnectionManager::disconnect(const QString& id) {
    auto* conn = connections_.value(id, nullptr);
    if (conn == nullptr) { return; }
    const auto server = conn->server();
    const auto cid = conn->connectionId();
    const auto proof = proofFor(id);
    conn->markDisconnected();
    clearRetry(id);
    if (cid.has_value()) {
        http_->deleteSession(server.ip, server.httpPort, *cid, deviceId_, proof,
                             [](int, bool, const QString&) {});
    }
}

void WifiConnectionManager::forget(const QString& id) {
    // Forget also self-unpairs server-side (best-effort): the satellite drops
    // this deviceId so its operator list stays truthful. Needs the proof, so
    // it must run before the key is deleted.
    auto* conn = connections_.value(id, nullptr);
    if (conn != nullptr) {
        const auto server = conn->server();
        const auto proof = proofFor(id);
        if (!proof.isEmpty()) {
            http_->unpair(server.ip, server.httpPort, deviceId_, proof,
                          [](int, bool, const QString&) {});
        }
    }
    disconnect(id);
    store_->forget(id);
    clearRetry(id);
    if (auto* taken = connections_.take(id)) {
        taken->deleteLater();
        emit poolChanged();
    }
}

void WifiConnectionManager::autoReconnectAll() {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (const auto& r : store_->remembered()) {
        auto* existing = connections_.value(r.id, nullptr);
        const bool idle = existing == nullptr || existing->state() == SessionState::Idle;
        if (!idle) { continue; }
        // Respect the backoff curve + the replaced-session suppression; a
        // user-initiated connect clears both.
        const auto it = retry_.constFind(r.id);
        if (it != retry_.constEnd() && (it->suppressed || now < it->nextRetryAtMs)) { continue; }
        connectTo(r.toDiscovered(), ConnectIntent::AutoReconnect);
    }
}

} // namespace dish::net
