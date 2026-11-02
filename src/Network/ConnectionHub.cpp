// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "ConnectionHub.h"

#include <QSet>

#include <algorithm>

namespace dish::net {

ConnectionHub::ConnectionHub(WifiConnectionManager* wifi, ConnectionStore* store, QObject* parent)
    : QObject(parent), wifi_(wifi), store_(store) {
    QObject::connect(wifi_, &WifiConnectionManager::poolChanged, this, &ConnectionHub::rebuild);
    // A scan moves Idle paired entries between Ready and Saved, so every row's
    // LinkState has to be re-derived.
    QObject::connect(wifi_, &WifiConnectionManager::discoveredChanged, this,
                     &ConnectionHub::rebuild);
    // Otherwise a rejected controller-add leaves a phantom binding: the slot card
    // claims it is bound while the satellite has no controller for it.
    QObject::connect(wifi_, &WifiConnectionManager::slotRegistrationFailed, this,
                     &ConnectionHub::unbind);
    rebuild();
}

namespace {

// An Idle or absent session is Ready when discovery can still see the host and Saved when it
// cannot: both are "not connected", but only one of them is one tap away.
models::LinkState idleState(const QSet<QString>& discoveredIds, const QString& id) {
    return discoveredIds.contains(id) ? models::LinkState::Ready : models::LinkState::Saved;
}

models::LinkState liveStateOf(const SessionState state, const QSet<QString>& discoveredIds,
                              const QString& id) {
    switch (state) {
    case SessionState::Live:
        return models::LinkState::Connected;
    case SessionState::Linking:
        return models::LinkState::Connecting;
    case SessionState::Faltering:
        // Entered at >=2 consecutive missed acks, recovers on the next.
        return models::LinkState::Unstable;
    case SessionState::Stale:
        // The heartbeat dropped or a reconnect's pair came back AuthRequired. The chip reads
        // "Needs pairing" while the manager keeps retrying silently.
        return models::LinkState::Stale;
    case SessionState::Idle:
        return idleState(discoveredIds, id);
    }
    // Unreachable: the switch is exhaustive over SessionState and carries no default, so a new
    // state is a compile error rather than a silent Saved.
    return models::LinkState::Saved;
}

} // namespace

// Every id the screen can show: one that has a live connection object, one that is only
// remembered, or both.
QSet<QString>
ConnectionHub::knownIds(const QHash<QString, models::RememberedWifi>& remembered) const {
    QSet<QString> ids;
    for (auto it = wifi_->connections().begin(); it != wifi_->connections().end(); ++it) {
        ids.insert(it.key());
    }
    for (auto it = remembered.begin(); it != remembered.end(); ++it) { ids.insert(it.key()); }
    return ids;
}

// The slot this connection is bound to, if any. The table is keyed the other way round, because
// a slot has one destination and a destination can be offered to several slots.
std::optional<QString> ConnectionHub::boundSlotFor(const QString& id) const {
    for (auto it = bindings_.begin(); it != bindings_.end(); ++it) {
        if (it.value() == id) { return it.key(); }
    }
    return std::nullopt;
}

// Null for an id whose server record is not usable, which is how a half-written remembered entry
// leaves the list rather than showing as a nameless row.
std::optional<models::ConnectionSummary>
ConnectionHub::summaryFor(const QString& id,
                          const QHash<QString, models::RememberedWifi>& remembered,
                          const QSet<QString>& discoveredIds) const {
    auto* conn = wifi_->get(id);
    const models::DiscoveredServer server =
        (conn != nullptr) ? conn->server() : remembered.value(id).toDiscovered();
    if (!server.isValid()) { return std::nullopt; }

    models::ConnectionSummary s;
    s.id = id;
    s.label = server.name.isEmpty() ? server.ip : server.name;
    s.detail = QStringLiteral("%1 • UDP %2").arg(server.ip).arg(server.udpPort);
    s.live = (conn != nullptr) ? liveStateOf(conn->state(), discoveredIds, id)
                               : idleState(discoveredIds, id);
    s.boundSlotId = boundSlotFor(id);
    return s;
}

void ConnectionHub::rebuild() {
    QHash<QString, models::RememberedWifi> remembered;
    for (const auto& r : store_->remembered()) { remembered.insert(r.id, r); }

    QSet<QString> discoveredIds;
    for (const auto& s : wifi_->discoveredServers()) { discoveredIds.insert(s.id()); }

    const QSet<QString> ids = knownIds(remembered);
    QList<models::ConnectionSummary> out;
    out.reserve(ids.size());
    for (const auto& id : ids) {
        if (auto s = summaryFor(id, remembered, discoveredIds)) { out.append(std::move(*s)); }
    }
    std::sort(out.begin(), out.end(),
              [](const auto& a, const auto& b) { return a.label < b.label; });
    summaries_ = std::move(out);
    emit changed();
}

QHash<QString, QString> ConnectionHub::bindings() const {
    std::lock_guard<thread::TrackedMutex> lock(bindingsMtx_);
    return bindings_;
}

void ConnectionHub::publishBindings(QHash<QString, QString> next) {
    std::lock_guard<thread::TrackedMutex> lock(bindingsMtx_);
    bindings_ = std::move(next);
}

std::optional<models::ConnectionSummary> ConnectionHub::summary(const QString& id) const {
    for (const auto& s : summaries_) {
        if (s.id == id) { return s; }
    }
    return std::nullopt;
}

void ConnectionHub::bind(const QString& slotId, const QString& connectionId) {
    QHash<QString, QString> current = bindings_;
    QString priorSlot;
    for (auto it = current.begin(); it != current.end(); ++it) {
        if (it.value() == connectionId && it.key() != slotId) {
            priorSlot = it.key();
            break;
        }
    }
    if (!priorSlot.isEmpty()) {
        current.remove(priorSlot);
        if (auto* prior = wifi_->get(connectionId)) { prior->detachSlot(); }
    }
    current.insert(slotId, connectionId);
    publishBindings(std::move(current));
    rebuild();
    const bool hasLightbar = lightbarCapabilityFn_ && lightbarCapabilityFn_(slotId);
    const bool hasMotion = motionCapabilityFn_ && motionCapabilityFn_(slotId);
    const bool hasRumble = rumbleCapabilityFn_ && rumbleCapabilityFn_(slotId);
    const bool hasTriggerEffects =
        triggerEffectsCapabilityFn_ && triggerEffectsCapabilityFn_(slotId);
    const bool hasPlayerLeds = playerLedsCapabilityFn_ && playerLedsCapabilityFn_(slotId);
    const bool hasMic = micCapabilityFn_ && micCapabilityFn_(slotId);
    const bool hasSpeaker = speakerCapabilityFn_ && speakerCapabilityFn_(slotId);
    const bool hasHapticAudio = hapticAudioCapabilityFn_ && hapticAudioCapabilityFn_(slotId);
    const int controllerType = controllerTypeFn_ ? controllerTypeFn_(slotId) : 0;
    const std::uint8_t touchpadMode =
        touchpadModeFn_ ? touchpadModeFn_(slotId) : proto::kTouchpadModeOff;
    if (auto* c = wifi_->get(connectionId)) {
        c->attachSlot(slotId, controllerType, hasLightbar, hasMotion, hasRumble, touchpadMode,
                      hasTriggerEffects, hasPlayerLeds, hasMic, hasSpeaker, hasHapticAudio);
    }
}

void ConnectionHub::unbind(const QString& slotId) {
    if (!bindings_.contains(slotId)) { return; }
    QHash<QString, QString> remaining = bindings_;
    const auto cid = remaining.take(slotId);
    publishBindings(std::move(remaining));
    if (auto* c = wifi_->get(cid)) { c->detachSlot(); }
    rebuild();
}

std::optional<models::ConnectionSummary>
ConnectionHub::boundConnection(const QString& slotId) const {
    const auto cid = bindings_.value(slotId);
    if (cid.isEmpty()) { return std::nullopt; }
    return summary(cid);
}

} // namespace dish::net
