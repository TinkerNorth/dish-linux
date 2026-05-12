// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "WifiConnectionManager.h"

#include "LANDiscovery.h"
#include "PairingClient.h"
#include "Util/Hex.h"

#include <QHostInfo>
#include <QtConcurrent/QtConcurrent>

#include <type_traits>
#include <variant>

namespace dish::net {

namespace {

ConnectionEvent makeError(const QString& msg) { return {ConnectionEventKind::Error, {}, msg}; }

ConnectionEvent pairingRequired(const models::DiscoveredServer& s) {
    return {ConnectionEventKind::PairingRequired, s, {}};
}

} // namespace

WifiConnectionManager::WifiConnectionManager(ConnectionStore* store, QObject* parent)
    : QObject(parent), store_(store), http_(new HTTPClient(this)) {
    deviceId_ = store_->getOrCreateDeviceId();
    deviceName_ = QHostInfo::localHostName();
    if (deviceName_.isEmpty()) { deviceName_ = QStringLiteral("Linux"); }
}

WifiConnectionManager::~WifiConnectionManager() {
    for (auto* c : connections_) { c->markDisconnected(); }
}

void WifiConnectionManager::startDiscovery() {
    if (scanning_) { return; }
    scanning_ = true;
    emit scanningChanged();
    auto* watcher = new QFutureWatcher<QList<models::DiscoveredServer>>(this);
    QObject::connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher] {
        discovered_ = watcher->result();
        scanning_ = false;
        emit discoveredChanged();
        emit scanningChanged();
        if (discovered_.isEmpty()) {
            emit connectionEvent(
                makeError(QStringLiteral("No servers found — check your network")));
        }
        watcher->deleteLater();
    });
    watcher->setFuture(QtConcurrent::run([] { return LANDiscovery::discover(); }));
}

WifiConnection* WifiConnectionManager::ensureConnection(const models::DiscoveredServer& server) {
    const auto id = WifiConnection::idFor(server);
    if (auto* existing = connections_.value(id, nullptr)) { return existing; }
    auto* conn = new WifiConnection(id, server, this);
    connections_.insert(id, conn);
    QObject::connect(conn, &WifiConnection::changed, this, &WifiConnectionManager::poolChanged);
    emit poolChanged();
    return conn;
}

void WifiConnectionManager::connectTo(const models::DiscoveredServer& server) {
    auto* conn = ensureConnection(server);
    if (conn->state() == WifiState::Connected || conn->state() == WifiState::Connecting) {
        conn->updateServer(server);
        return;
    }
    conn->updateServer(server);
    conn->markConnecting();
    pairAndConnect(conn, server, QString());
}

void WifiConnectionManager::pairWithPin(const models::DiscoveredServer& server,
                                        const QString& pin) {
    auto* conn = ensureConnection(server);
    conn->markConnecting();
    pairAndConnect(conn, server, pin);
}

void WifiConnectionManager::pairAndConnect(WifiConnection* conn,
                                           const models::DiscoveredServer& server,
                                           const QString& pin) {
    // Auto-reconnect fast path (pin.isEmpty()): if we already have a shared
    // key saved for this server, skip the TCP pair handshake entirely and
    // go straight to openSession. A moved/offline server then fails fast in
    // the HTTP layer instead of bouncing through pair → PairingRequired and
    // trapping the user behind a PIN prompt that can't be satisfied. Mirrors
    // dish-android PR #43.
    if (pin.isEmpty()) {
        const auto saved = store_->sharedKey(WifiConnection::idFor(server));
        if (saved.has_value() && saved->size() == 64) {
            openSession(conn, server);
            return;
        }
    }
    const QString did = deviceId_;
    const QString dname = deviceName_;
    auto* watcher = new QFutureWatcher<models::PairResponse>(this);
    QObject::connect(
        watcher, &QFutureWatcherBase::finished, this, [this, watcher, conn, server, pin] {
            const auto pair = watcher->result();
            watcher->deleteLater();
            const auto outcome = PairingClient::classify(pair);
            std::visit(
                [&](auto&& arm) {
                    using T = std::decay_t<decltype(arm)>;
                    if constexpr (std::is_same_v<T, PairingClient::Success>) {
                        store_->setSharedKey(arm.sharedKeyHex, WifiConnection::idFor(server));
                        openSession(conn, server);
                    } else if constexpr (std::is_same_v<T, PairingClient::AuthRequired>) {
                        conn->markDisconnected();
                        if (pin.isEmpty()) {
                            emit connectionEvent(pairingRequired(server));
                        } else {
                            emit connectionEvent(makeError(
                                pair.error.value_or(QStringLiteral("Pairing failed"))));
                        }
                    } else if constexpr (std::is_same_v<T, PairingClient::Unreachable>) {
                        conn->markDisconnected();
                        emit connectionEvent(makeError(
                            QStringLiteral("Server unreachable — has it moved networks? (%1)")
                                .arg(arm.message)));
                    }
                },
                outcome);
        });
    watcher->setFuture(QtConcurrent::run([server, did, dname, pin] {
        return PairingClient::pair(server.ip, server.pairPort, did, dname, pin);
    }));
}

void WifiConnectionManager::openSession(WifiConnection* conn,
                                        const models::DiscoveredServer& server) {
    const auto id = WifiConnection::idFor(server);
    const auto keyHex = store_->sharedKey(id);
    if (!keyHex.has_value() || keyHex->size() != 64) {
        conn->markDisconnected();
        emit connectionEvent(makeError(QStringLiteral("No shared key — re-pair needed")));
        return;
    }
    const auto keyBytes = util::fromHex(keyHex->toStdString());
    if (!keyBytes || keyBytes->size() != 32) {
        conn->markDisconnected();
        emit connectionEvent(makeError(QStringLiteral("Bad shared key — re-pair needed")));
        return;
    }
    std::array<std::uint8_t, 32> key{};
    std::copy_n(keyBytes->begin(), 32, key.begin());

    http_->connectAsync(
        server.ip, server.httpPort, deviceId_,
        [this, conn, server, key](const models::ConnectResponse& resp) {
            if (!resp.connectionId.has_value() || !resp.token.has_value()) {
                conn->markDisconnected();
                emit connectionEvent(
                    makeError(QStringLiteral("Error: %1")
                                  .arg(resp.error.value_or(QStringLiteral("connection failed")))));
                return;
            }
            const auto tok = util::fromHex(resp.token->toStdString());
            if (!tok || tok->size() != 4) {
                conn->markDisconnected();
                emit connectionEvent(makeError(QStringLiteral("Bad token from server")));
                return;
            }
            std::array<std::uint8_t, 4> token{};
            std::copy_n(tok->begin(), 4, token.begin());

            auto client = std::make_shared<SatelliteClient>();
            if (!client->openSocket(server.ip.toStdString(), server.udpPort)) {
                conn->markDisconnected();
                return;
            }
            client->setConnectionParams(token, key);
            store_->remember(server);
            const QString cid = *resp.connectionId;
            const QString connId = conn->id();
            conn->markConnected(client, cid, [this, connId] { disconnect(connId); });
        });
}

void WifiConnectionManager::disconnect(const QString& id) {
    auto* conn = connections_.value(id, nullptr);
    if (conn == nullptr) { return; }
    const auto server = conn->server();
    const auto cid = conn->connectionId();
    conn->markDisconnected();
    if (cid.has_value()) {
        http_->disconnectAsync(server.ip, server.httpPort, *cid, deviceId_,
                               [](const models::ConnectResponse&) {});
    }
}

void WifiConnectionManager::forget(const QString& id) {
    disconnect(id);
    store_->forget(id);
    if (auto* conn = connections_.take(id)) {
        conn->deleteLater();
        emit poolChanged();
    }
}

void WifiConnectionManager::autoReconnectAll() {
    for (const auto& r : store_->remembered()) {
        auto* existing = connections_.value(r.id, nullptr);
        if (existing == nullptr || existing->state() != WifiState::Connected) {
            connectTo(r.toDiscovered());
        }
    }
}

} // namespace dish::net
