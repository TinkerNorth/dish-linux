// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "ConnectionHub.h"

#include <QSet>

#include <algorithm>

namespace dish::net {

ConnectionHub::ConnectionHub(WifiConnectionManager* wifi, ConnectionStore* store, QObject* parent)
    : QObject(parent), wifi_(wifi), store_(store) {
    QObject::connect(wifi_, &WifiConnectionManager::poolChanged, this, &ConnectionHub::rebuild);
    // A discovery-result delta also splits paired Idle entries into Ready
    // (in the current scan) vs Saved (not in the scan) \u2014 rebuild on it too.
    QObject::connect(wifi_, &WifiConnectionManager::discoveredChanged, this,
                     &ConnectionHub::rebuild);
    QObject::connect(wifi_, &WifiConnectionManager::slotRegistrationFailed, this,
                     &ConnectionHub::unbind);
    rebuild();
}

void ConnectionHub::rebuild() {
    QHash<QString, models::RememberedWifi> remembered;
    for (const auto& r : store_->remembered()) { remembered.insert(r.id, r); }

    // Ids in the current discovery scan \u2014 the "Presence axis = seen" predicate
    // used to split an Idle paired entry into Ready (we see it on the network)
    // vs Saved (we don't). Mirrors dish-android's discoveredIdSet. Unpaired
    // entries that are *only* in the scan (not remembered, not in the live
    // pool) are handled by ConnectionsPage's separate discovered list \u2014 same
    // split dish-android and dish-windows use.
    QSet<QString> discoveredIds;
    for (const auto& s : wifi_->discoveredServers()) { discoveredIds.insert(s.id()); }

    QSet<QString> ids;
    for (auto it = wifi_->connections().begin(); it != wifi_->connections().end(); ++it) {
        ids.insert(it.key());
    }
    for (auto it = remembered.begin(); it != remembered.end(); ++it) { ids.insert(it.key()); }

    QList<models::ConnectionSummary> out;
    out.reserve(ids.size());
    for (const auto& id : ids) {
        auto* conn = wifi_->get(id);
        const models::DiscoveredServer server =
            (conn != nullptr) ? conn->server() : remembered.value(id).toDiscovered();
        if (!server.isValid()) { continue; }
        // Derive LinkState from the wire-level SessionState plus whether this
        // id is currently in the discovery set. Mirrors dish-android's
        // ConnectionHub.buildSatelliteSummary.
        //
        // - SessionState::Live      -> LinkState::Connected
        // - SessionState::Linking   -> LinkState::Connecting
        // - SessionState::Faltering -> LinkState::Unstable (not yet reachable;
        //   native exposes only binary alive)
        // - SessionState::Idle / no conn:
        //     in discoveredIds      -> LinkState::Ready
        //     not in discoveredIds  -> LinkState::Saved
        //
        // TODO(LinkState::Stale): a server-side forget should land us in
        // LinkState::Stale, but detecting that requires the server to return a
        // `PAIRING_UNKNOWN` error so we can distinguish "peer forgot us" from
        // a transient unreachability. Until that protocol bit lands, a
        // forgotten device falls through to Saved/Ready and the user only
        // sees connect failures.
        models::LinkState live = models::LinkState::Saved;
        if (conn != nullptr) {
            switch (conn->state()) {
            case SessionState::Live:
                live = models::LinkState::Connected;
                break;
            case SessionState::Linking:
                live = models::LinkState::Connecting;
                break;
            case SessionState::Faltering:
                // TODO(LinkState::Unstable): native alive-poll currently only
                // exposes a binary "is alive" and flips Live -> Idle directly
                // when misses hit the death threshold, so this arm is defined
                // but never taken. Once the miss count is exposed, the
                // alive-poll should bump SessionState to Faltering instead.
                live = models::LinkState::Unstable;
                break;
            case SessionState::Idle:
                live = discoveredIds.contains(id) ? models::LinkState::Ready
                                                  : models::LinkState::Saved;
                break;
            }
        } else {
            live = discoveredIds.contains(id) ? models::LinkState::Ready : models::LinkState::Saved;
        }
        std::optional<QString> bound;
        for (auto it = bindings_.begin(); it != bindings_.end(); ++it) {
            if (it.value() == id) {
                bound = it.key();
                break;
            }
        }
        const QString label = server.name.isEmpty() ? server.ip : server.name;
        models::ConnectionSummary s;
        s.id = id;
        s.label = label;
        s.detail = QStringLiteral("%1 \u2022 UDP %2").arg(server.ip).arg(server.udpPort);
        s.live = live;
        s.boundSlotId = bound;
        out.append(s);
    }
    std::sort(out.begin(), out.end(),
              [](const auto& a, const auto& b) { return a.label < b.label; });
    summaries_ = std::move(out);
    emit changed();
}

std::optional<models::ConnectionSummary> ConnectionHub::summary(const QString& id) const {
    for (const auto& s : summaries_) {
        if (s.id == id) { return s; }
    }
    return std::nullopt;
}

ConnectionHub::ReportSender ConnectionHub::reportSenderForSlot(const QString& slotId) const {
    const auto cid = bindings_.value(slotId);
    if (cid.isEmpty()) { return {}; }
    auto* conn = wifi_->get(cid);
    if (conn == nullptr) { return {}; }
    // Capture by raw pointer — WifiConnection is parented to the manager and
    // outlives the input thread (Manager dtor disconnects all sessions before
    // destruction). The internal ClientRef provides per-call thread safety.
    return [conn](std::uint16_t buttons, std::uint8_t lt, std::uint8_t rt, std::int16_t lx,
                  std::int16_t ly, std::int16_t rx,
                  std::int16_t ry) { conn->sendReport(buttons, lt, rt, lx, ly, rx, ry); };
}

ConnectionHub::MotionSender ConnectionHub::motionSenderForSlot(const QString& slotId) const {
    const auto cid = bindings_.value(slotId);
    if (cid.isEmpty()) { return {}; }
    auto* conn = wifi_->get(cid);
    if (conn == nullptr) { return {}; }
    return [conn](std::int16_t gx, std::int16_t gy, std::int16_t gz, std::int16_t ax,
                  std::int16_t ay, std::int16_t az,
                  std::uint32_t dtUs) { conn->sendMotion(gx, gy, gz, ax, ay, az, dtUs); };
}

ConnectionHub::BatterySender ConnectionHub::batterySenderForSlot(const QString& slotId) const {
    const auto cid = bindings_.value(slotId);
    if (cid.isEmpty()) { return {}; }
    auto* conn = wifi_->get(cid);
    if (conn == nullptr) { return {}; }
    return [conn](std::uint8_t level, std::uint8_t status) { conn->sendBattery(level, status); };
}

ConnectionHub::TouchpadSender ConnectionHub::touchpadSenderForSlot(const QString& slotId) const {
    const auto cid = bindings_.value(slotId);
    if (cid.isEmpty()) { return {}; }
    auto* conn = wifi_->get(cid);
    if (conn == nullptr) { return {}; }
    return [conn](bool f0a, std::uint8_t f0id, std::int16_t f0x, std::int16_t f0y, bool f1a,
                  std::uint8_t f1id, std::int16_t f1x, std::int16_t f1y, bool button) {
        conn->sendTouchpad(f0a, f0id, f0x, f0y, f1a, f1id, f1x, f1y, button);
    };
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
    bindings_ = current;
    rebuild();
    // Resolve the slot's pad capabilities so the controller-add can advertise
    // CAP_LIGHTBAR / CAP_MOTION and MSG_CONTROLLER_TYPE can declare the real
    // Xbox-vs-PlayStation kind. The resolvers are absent in tests / before
    // the bridge is wired — treat that as "no lightbar / no motion / Xbox".
    const bool hasLightbar = lightbarCapabilityFn_ && lightbarCapabilityFn_(slotId);
    const bool hasMotion = motionCapabilityFn_ && motionCapabilityFn_(slotId);
    const int controllerType = controllerTypeFn_ ? controllerTypeFn_(slotId) : 0;
    if (auto* c = wifi_->get(connectionId)) {
        c->attachSlot(slotId, controllerType, hasLightbar, hasMotion);
    }
}

void ConnectionHub::unbind(const QString& slotId) {
    if (!bindings_.contains(slotId)) { return; }
    const auto cid = bindings_.take(slotId);
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
