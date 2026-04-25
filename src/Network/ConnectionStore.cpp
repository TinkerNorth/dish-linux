// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "ConnectionStore.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QUuid>

namespace dish::net {

namespace {

constexpr const char* kDeviceIdKey = "deviceId";
constexpr const char* kWifiListKey = "wifi_list";
constexpr const char* kSharedKeyPrefix = "wifi_shared_key/";

} // namespace

ConnectionStore::ConnectionStore(std::unique_ptr<QSettings> settings) {
    settings_ = settings
                    ? std::move(settings)
                    : std::make_unique<QSettings>(QStringLiteral("Dish"), QStringLiteral("Dish"));
}

QString ConnectionStore::getOrCreateDeviceId() {
    const auto existing = settings_->value(QLatin1String(kDeviceIdKey)).toString();
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

void ConnectionStore::remember(const models::DiscoveredServer& server) {
    const QString id = server.id();
    auto list = remembered();
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
    list.append(r);
    persist(list);
}

void ConnectionStore::forget(const QString& id) {
    auto list = remembered();
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
    const auto v = settings_->value(QLatin1String(kSharedKeyPrefix) + id).toString();
    if (v.isEmpty()) { return std::nullopt; }
    return v;
}

void ConnectionStore::setSharedKey(const QString& keyHex, const QString& id) {
    settings_->setValue(QLatin1String(kSharedKeyPrefix) + id, keyHex);
}

} // namespace dish::net
