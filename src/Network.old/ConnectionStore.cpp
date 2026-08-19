// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "ConnectionStore.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QUuid>

#include <algorithm>

namespace dish::net {

namespace {

constexpr const char* kDeviceIdKey = "deviceId";
constexpr const char* kWifiListKey = "wifi_list";
constexpr const char* kSharedKeyPrefix = "wifi_shared_key/";
constexpr const char* kCertPinPrefix = "cert_pin/";

bool isBlank(const QString& s) { return s.trimmed().isEmpty(); }

} // namespace

ConnectionStore::ConnectionStore(std::unique_ptr<QSettings> settings) {
    settings_ = settings
                    ? std::move(settings)
                    : std::make_unique<QSettings>(QStringLiteral("Dish"), QStringLiteral("Dish"));
}

QString ConnectionStore::getOrCreateDeviceId() {
    auto existing = settings_->value(QLatin1String(kDeviceIdKey)).toString();
    if (!existing.isEmpty()) { return existing; }
    const auto fresh =
        QUuid::createUuid().toString(QUuid::WithoutBraces).remove(QLatin1Char('-')).toLower();
    settings_->setValue(QLatin1String(kDeviceIdKey), fresh);
    return fresh;
}

QList<models::RememberedWifi> ConnectionStore::remembered() const {
    const auto raw = settings_->value(QLatin1String(kWifiListKey)).toByteArray();
    if (raw.isEmpty()) { return {}; }
    const auto doc = QJsonDocument::fromJson(raw);
    if (!doc.isArray()) { return {}; }
    return models::rememberedListFromJson(doc.array());
}

bool ConnectionStore::refreshKnownBox(const models::DiscoveredServer& server) {
    // The server advertised no machineId. Find an existing STABLE row (one
    // that DID advertise one) at the same address and refresh its name + ports
    // in place — don't mint an ip:port ghost beside it.
    auto list = remembered();
    for (auto& row : list) {
        if (!isBlank(row.machineId) && row.ip == server.ip && row.udpPort == server.udpPort) {
            models::RememberedWifi refreshed = row;
            refreshed.name = server.name;
            refreshed.pairPort = server.pairPort;
            refreshed.httpPort = server.httpPort;
            if (refreshed != row) {
                row = refreshed;
                persist(list);
            }
            return true;
        }
    }
    return false;
}

void ConnectionStore::collapseLegacyGhosts(const models::DiscoveredServer& server,
                                           const QString& id) {
    // The box just gained a stable id. For every legacy row (no machineId) at
    // the same address: carry its pairing key forward to the stable id (the
    // stable row's own key wins if it already has one), then drop the ghost's
    // key + row.
    auto list = remembered();
    bool changed = false;
    for (const auto& row : list) {
        if (isBlank(row.machineId) && row.ip == server.ip && row.udpPort == server.udpPort) {
            if (!sharedKey(id).has_value()) {
                if (const auto ghostKey = sharedKey(row.id)) { setSharedKey(*ghostKey, id); }
            }
            settings_->remove(QLatin1String(kSharedKeyPrefix) + row.id);
            changed = true;
        }
    }
    if (changed) {
        list.erase(std::remove_if(list.begin(), list.end(),
                                  [&](const models::RememberedWifi& r) {
                                      return isBlank(r.machineId) && r.ip == server.ip &&
                                             r.udpPort == server.udpPort;
                                  }),
                   list.end());
        persist(list);
    }
}

void ConnectionStore::migratePinOnAddressChange(const std::optional<QString>& oldIp,
                                                const QString& newIp) {
    // The cert pin follows the box (pin keyed by IP). A pin already trusted at
    // the new address is NOT overwritten; the old-address pin is ALWAYS dropped.
    if (!oldIp.has_value() || *oldIp == newIp) { return; }
    if (!certPin(newIp).has_value()) {
        if (const auto oldPin = certPin(*oldIp)) { setCertPin(newIp, *oldPin); }
    }
    forgetCertPin(*oldIp);
}

void ConnectionStore::remember(const models::DiscoveredServer& server) {
    // A discovery result without a machineId never mints a fresh row beside a
    // stable one — it only refreshes a known stable row at the same address.
    if (isBlank(server.machineId) && refreshKnownBox(server)) { return; }

    const QString id = server.id();
    if (!isBlank(server.machineId)) { collapseLegacyGhosts(server, id); }

    auto list = remembered();
    std::optional<QString> oldIp;
    for (const auto& r : list) {
        if (r.id == id) {
            oldIp = r.ip;
            break;
        }
    }
    migratePinOnAddressChange(oldIp, server.ip);

    list.erase(std::remove_if(list.begin(), list.end(),
                              [&](const models::RememberedWifi& r) { return r.id == id; }),
               list.end());
    models::RememberedWifi r;
    r.id = id;
    r.name = server.name;
    r.ip = server.ip;
    r.udpPort = server.udpPort;
    r.pairPort = server.pairPort;
    r.httpPort = server.httpPort;
    r.machineId = server.machineId;
    list.append(r);
    persist(list);
}

void ConnectionStore::refreshFromDiscovery(const QList<models::DiscoveredServer>& discovered) {
    const auto rows = remembered();
    QSet<QString> knownIds;
    for (const auto& r : rows) { knownIds.insert(r.id); }

    for (const auto& server : discovered) {
        // A result without a machineId never re-points a remembered row.
        if (isBlank(server.machineId)) { continue; }
        bool eligible = knownIds.contains(server.id());
        if (!eligible) {
            // Or a legacy ip:port row this stable server is the upgrade of.
            for (const auto& r : rows) {
                if (isBlank(r.machineId) && r.ip == server.ip && r.udpPort == server.udpPort) {
                    eligible = true;
                    break;
                }
            }
        }
        if (eligible) { remember(server); }
    }
}

void ConnectionStore::forget(const QString& id) {
    auto list = remembered();
    // Pin is keyed by IP; drop it via the row's IP before the row goes.
    for (const auto& r : list) {
        if (r.id == id) {
            forgetCertPin(r.ip);
            break;
        }
    }
    list.erase(std::remove_if(list.begin(), list.end(),
                              [&](const models::RememberedWifi& r) { return r.id == id; }),
               list.end());
    persist(list);
    settings_->remove(QLatin1String(kSharedKeyPrefix) + id);
}

void ConnectionStore::persist(const QList<models::RememberedWifi>& list) {
    const auto doc = QJsonDocument(models::rememberedListToJson(list));
    settings_->setValue(QLatin1String(kWifiListKey), doc.toJson(QJsonDocument::Compact));
}

std::optional<QString> ConnectionStore::sharedKey(const QString& id) const {
    auto v = settings_->value(QLatin1String(kSharedKeyPrefix) + id).toString();
    if (v.isEmpty()) { return std::nullopt; }
    return v;
}

void ConnectionStore::setSharedKey(const QString& keyHex, const QString& id) {
    settings_->setValue(QLatin1String(kSharedKeyPrefix) + id, keyHex);
}

void ConnectionStore::forgetKey(const QString& id) {
    settings_->remove(QLatin1String(kSharedKeyPrefix) + id);
}

std::optional<QString> ConnectionStore::certPin(const QString& host) const {
    std::lock_guard<std::mutex> lock(pinMtx_);
    auto v = settings_->value(QLatin1String(kCertPinPrefix) + host).toString();
    if (v.isEmpty()) { return std::nullopt; }
    return v;
}

void ConnectionStore::setCertPin(const QString& host, const QString& fingerprintHex) {
    std::lock_guard<std::mutex> lock(pinMtx_);
    settings_->setValue(QLatin1String(kCertPinPrefix) + host, fingerprintHex);
}

void ConnectionStore::forgetCertPin(const QString& host) {
    std::lock_guard<std::mutex> lock(pinMtx_);
    settings_->remove(QLatin1String(kCertPinPrefix) + host);
}

} // namespace dish::net
