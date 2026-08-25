// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "repository/MoonlightHostRepository.h"

#include "repository/AppSettings.h"
#include "repository/SettingsKeys.h"

#include <QJsonArray>
#include <QJsonDocument>

#include <vector>

namespace dish::repository {

QJsonObject MoonlightHost::toJson() const {
    QJsonObject obj;
    obj.insert(QStringLiteral("uuid"), uuid);
    obj.insert(QStringLiteral("name"), name);
    obj.insert(QStringLiteral("address"), address);
    obj.insert(QStringLiteral("httpPort"), httpPort);
    obj.insert(QStringLiteral("httpsPort"), httpsPort);
    obj.insert(QStringLiteral("serverCertPem"), serverCertPem);
    obj.insert(QStringLiteral("lastAppId"), lastAppId);
    obj.insert(QStringLiteral("lastAppName"), lastAppName);
    obj.insert(QStringLiteral("controllerType"), controllerType);
    return obj;
}

std::optional<MoonlightHost> MoonlightHost::fromJson(const QJsonObject& obj) {
    MoonlightHost host;
    host.uuid = obj.value(QLatin1String("uuid")).toString();
    host.address = obj.value(QLatin1String("address")).toString();
    if (host.uuid.isEmpty() || host.address.isEmpty()) { return std::nullopt; }
    host.name = obj.value(QLatin1String("name")).toString();
    host.httpPort = obj.value(QLatin1String("httpPort")).toInt(47989);
    host.httpsPort = obj.value(QLatin1String("httpsPort")).toInt(47984);
    host.serverCertPem = obj.value(QLatin1String("serverCertPem")).toString();
    host.lastAppId = obj.value(QLatin1String("lastAppId")).toString();
    host.lastAppName = obj.value(QLatin1String("lastAppName")).toString();
    host.controllerType =
        obj.value(QLatin1String("controllerType")).toInt(kMoonlightControllerTypeAuto);
    return host;
}

MoonlightHostRepository::MoonlightHostRepository(std::shared_ptr<QSettings> settings)
    : settings_(settings ? std::move(settings) : repository::makeSettings()) {}

QHash<QString, MoonlightHost> MoonlightHostRepository::load() const {
    QHash<QString, MoonlightHost> hosts;
    const auto raw = settings_->value(QLatin1String(keys::kMoonlightHostListKey)).toByteArray();
    if (raw.isEmpty()) { return hosts; }
    const auto doc = QJsonDocument::fromJson(raw);
    // A JSON object keyed by storage key; the array form is the legacy shape a
    // pre-release build wrote, still read so an in-place upgrade keeps rows.
    if (doc.isObject()) {
        const QJsonObject obj = doc.object();
        for (auto it = obj.constBegin(); it != obj.constEnd(); ++it) {
            if (auto host = MoonlightHost::fromJson(it.value().toObject())) {
                // operator[], not insert: QHash::insert takes the value by
                // const reference, so a std::move into it would copy.
                hosts[it.key()] = std::move(*host);
            }
        }
    } else if (doc.isArray()) {
        for (const auto& entry : doc.array()) {
            if (auto host = MoonlightHost::fromJson(entry.toObject())) {
                // As above; the key is copied out first so that it cannot be
                // read out of the host the same statement moves from.
                const QString uuid = host->uuid;
                hosts[uuid] = std::move(*host);
            }
        }
    }
    return hosts;
}

void MoonlightHostRepository::store(const QHash<QString, MoonlightHost>& hosts) {
    QJsonObject obj;
    for (auto it = hosts.constBegin(); it != hosts.constEnd(); ++it) {
        obj.insert(it.key(), it.value().toJson());
    }
    settings_->setValue(QLatin1String(keys::kMoonlightHostListKey),
                        QJsonDocument(obj).toJson(QJsonDocument::Compact));
    settings_->sync();
}

void MoonlightHostRepository::upsert(const MoonlightHost& host) {
    if (host.uuid.isEmpty()) { return; }
    std::lock_guard<std::mutex> lock(mutex_);
    auto hosts = load();
    const auto it = hosts.constFind(host.uuid);
    if (it == hosts.constEnd()) {
        hosts.insert(host.uuid, host);
    } else {
        MoonlightHost merged = host;
        // A re-discovery carries no pairing anchor or picks; keep the stored
        // ones rather than wiping them.
        if (merged.serverCertPem.isEmpty()) { merged.serverCertPem = it->serverCertPem; }
        if (merged.lastAppId.isEmpty()) {
            merged.lastAppId = it->lastAppId;
            merged.lastAppName = it->lastAppName;
        }
        if (merged.name.isEmpty()) { merged.name = it->name; }
        hosts.insert(host.uuid, merged);
    }
    store(hosts);
}

std::optional<MoonlightHost> MoonlightHostRepository::get(const QString& uuid) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto hosts = load();
    const auto it = hosts.constFind(uuid);
    if (it == hosts.constEnd()) { return std::nullopt; }
    return *it;
}

std::vector<MoonlightHost> MoonlightHostRepository::all() const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto hosts = load();
    std::vector<MoonlightHost> out;
    out.reserve(static_cast<std::size_t>(hosts.size()));
    for (const auto& host : hosts) { out.push_back(host); }
    return out;
}

void MoonlightHostRepository::put(const QString& uuid, const MoonlightHost& host) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto hosts = load();
    hosts.insert(uuid, host); // storage key authoritative, value stored verbatim
    store(hosts);
}

void MoonlightHostRepository::remove(const QString& uuid) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto hosts = load();
    hosts.remove(uuid);
    store(hosts);
}

void MoonlightHostRepository::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    settings_->remove(QLatin1String(keys::kMoonlightHostListKey));
    settings_->sync();
}

} // namespace dish::repository
