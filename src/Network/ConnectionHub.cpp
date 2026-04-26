// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "ConnectionHub.h"

#include <QSet>

#include <algorithm>

namespace dish::net {

ConnectionHub::ConnectionHub(WifiConnectionManager* wifi, ConnectionStore* store, QObject* parent)
    : QObject(parent), wifi_(wifi), store_(store) {
    QObject::connect(wifi_, &WifiConnectionManager::poolChanged, this, &ConnectionHub::rebuild);
    rebuild();
}

void ConnectionHub::rebuild() {
    QHash<QString, models::RememberedWifi> remembered;
    for (const auto& r : store_->remembered()) { remembered.insert(r.id, r); }

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
        models::ConnectionLive live = models::ConnectionLive::Idle;
        if (conn != nullptr) {
            switch (conn->state()) {
            case WifiState::Connected:
                live = models::ConnectionLive::Connected;
                break;
            case WifiState::Connecting:
                live = models::ConnectionLive::Connecting;
                break;
            case WifiState::Idle:
                live = models::ConnectionLive::Idle;
                break;
            }
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
    if (auto* c = wifi_->get(connectionId)) { c->attachSlot(slotId, /*controllerType=*/0); }
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
