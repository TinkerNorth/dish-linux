// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "composer/ConnectionCoordinator.h"

#include <algorithm>

namespace dish::composer {

namespace {

// Kept at the Qt boundary so the composer and reducers carry no Network
// dependency.
reducer::SessionPresence presenceOf(net::SessionState state) {
    switch (state) {
    case net::SessionState::Idle:
        return reducer::SessionPresence::Idle;
    case net::SessionState::Linking:
        return reducer::SessionPresence::Linking;
    case net::SessionState::Live:
        return reducer::SessionPresence::Live;
    case net::SessionState::Faltering:
        return reducer::SessionPresence::Faltering;
    case net::SessionState::Stale:
    default:
        return reducer::SessionPresence::Stale;
    }
}

} // namespace

ConnectionCoordinator::ConnectionCoordinator(net::WifiConnectionManager* wifi,
                                             net::ConnectionHub* hub, QObject* parent)
    : QObject(parent), wifi_(wifi), hub_(hub), sessions_({}), remembered_({}), discoveredIds_({}),
      bindings_({}), staleIds_({}) {
    composer_ = std::make_unique<ConnectionsComposer>(sessions_, remembered_, discoveredIds_,
                                                      bindings_, staleIds_);
    composerSub_ = composer_->state().subscribe(
        [this](const std::vector<ConnectionRow>&) { emit connectionsChanged(); },
        /*emitCurrent=*/false);

    QObject::connect(wifi_, &net::WifiConnectionManager::poolChanged, this, [this] {
        refreshSessions();
        refreshRemembered();
    });
    QObject::connect(wifi_, &net::WifiConnectionManager::discoveredChanged, this, [this] {
        refreshDiscovered();
        refreshRemembered();
    });
    // The 1 s latency tick refreshes sessions WITHOUT the poolChanged fan-out
    // (which re-runs the hub/AppModel rebuilds); distinct-until-changed then
    // passes only the readouts that actually moved.
    QObject::connect(wifi_, &net::WifiConnectionManager::poolTelemetryChanged, this,
                     [this] { refreshSessions(); });
    QObject::connect(hub_, &net::ConnectionHub::changed, this,
                     &ConnectionCoordinator::refreshBindings);

    // Eager, so a subscriber attached right after construction sees the current
    // world rather than waiting for the first signal.
    refreshSessions();
    refreshRemembered();
    refreshDiscovered();
    refreshBindings();
}

ConnectionCoordinator::~ConnectionCoordinator() = default;

void ConnectionCoordinator::refreshSessions() {
    std::vector<SessionSnapshot> next;
    std::vector<std::string> stale;
    const auto& conns = wifi_->connections();
    next.reserve(static_cast<std::size_t>(conns.size()));
    for (auto it = conns.begin(); it != conns.end(); ++it) {
        const net::WifiConnection* conn = it.value();
        if (conn == nullptr) { continue; }
        const auto& server = conn->server();
        SessionSnapshot s;
        s.id = conn->id().toStdString();
        s.presence = presenceOf(conn->state());
        s.name = server.name.toStdString();
        s.ip = server.ip.toStdString();
        s.udpPort = server.udpPort;
        // Already display-rounded by the alive-poll, so exact == keys
        // distinct-until-changed on visible moves only.
        s.latencyOneWayMs = conn->latencyOneWayMs();
        s.latencySamples = conn->latencySamples();
        next.push_back(std::move(s));
        // Mark the id stale too, so a remembered row at the same id keeps
        // reading "Needs pairing" after the live row is gone.
        if (conn->state() == net::SessionState::Stale) {
            stale.push_back(conn->id().toStdString());
        }
    }
    std::sort(next.begin(), next.end(),
              [](const SessionSnapshot& a, const SessionSnapshot& b) { return a.id < b.id; });
    std::sort(stale.begin(), stale.end());
    sessions_.set(std::move(next));
    staleIds_.set(std::move(stale));
}

void ConnectionCoordinator::refreshRemembered() {
    std::vector<RememberedSnapshot> next;
    const auto remembered = wifi_->remembered();
    next.reserve(static_cast<std::size_t>(remembered.size()));
    for (const auto& r : remembered) {
        RememberedSnapshot s;
        s.id = r.id.toStdString();
        s.name = r.name.toStdString();
        s.ip = r.ip.toStdString();
        s.udpPort = r.udpPort;
        next.push_back(std::move(s));
    }
    std::sort(next.begin(), next.end(),
              [](const RememberedSnapshot& a, const RememberedSnapshot& b) { return a.id < b.id; });
    remembered_.set(std::move(next));
}

void ConnectionCoordinator::refreshDiscovered() {
    std::vector<std::string> next;
    const auto discovered = wifi_->discoveredServers();
    next.reserve(static_cast<std::size_t>(discovered.size()));
    for (const auto& s : discovered) { next.push_back(s.id().toStdString()); }
    std::sort(next.begin(), next.end());
    next.erase(std::unique(next.begin(), next.end()), next.end());
    discoveredIds_.set(std::move(next));
}

void ConnectionCoordinator::refreshBindings() {
    std::vector<Binding> next;
    const auto bindings = hub_->bindings();
    next.reserve(static_cast<std::size_t>(bindings.size()));
    for (auto it = bindings.cbegin(); it != bindings.cend(); ++it) {
        next.push_back({it.key().toStdString(), it.value().toStdString()});
    }
    std::sort(next.begin(), next.end(),
              [](const Binding& a, const Binding& b) { return a.slotId < b.slotId; });
    bindings_.set(std::move(next));
}

std::optional<ConnectionRow> ConnectionCoordinator::summary(const std::string& id) const {
    const auto rows = composer_->state().value();
    for (const auto& r : rows) {
        if (r.id == id) { return r; }
    }
    return std::nullopt;
}

void ConnectionCoordinator::bind(const QString& slotId, const QString& connectionId) {
    hub_->bind(slotId, connectionId);
}

void ConnectionCoordinator::unbind(const QString& slotId) { hub_->unbind(slotId); }

void ConnectionCoordinator::forgetConnection(const QString& connectionId) {
    // Unbind before forgetting: the detach needs the host still on file.
    const auto bindings = hub_->bindings();
    for (auto it = bindings.cbegin(); it != bindings.cend(); ++it) {
        if (it.value() == connectionId) { hub_->unbind(it.key()); }
    }
    wifi_->forget(connectionId);
    // forget() only fires poolChanged when there was a LIVE connection to drop,
    // so a remembered-but-offline satellite would otherwise linger in the list.
    refreshRemembered();
    refreshSessions();
}

void ConnectionCoordinator::autoReconnectAll() { wifi_->autoReconnectAll(); }

void ConnectionCoordinator::reconnectConnection(const QString& connectionId) {
    // A currently-discovered endpoint has a guaranteed-current IP, so prefer it.
    for (const auto& s : wifi_->discoveredServers()) {
        if (s.id() == connectionId) {
            wifi_->connectTo(s);
            return;
        }
    }
    // Otherwise do both: a discovery pass relearns a box that moved, and the
    // last-known address is tried immediately in case it did not. Either way the
    // user never has to rescan by hand.
    wifi_->startDiscovery();
    for (const auto& r : wifi_->remembered()) {
        if (r.id == connectionId) {
            wifi_->connectTo(r.toDiscovered());
            return;
        }
    }
}

void ConnectionCoordinator::disconnectConnection(const QString& connectionId) {
    wifi_->disconnect(connectionId);
}

} // namespace dish::composer
