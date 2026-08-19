// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "repository/ConnectionStore.h"

#include "Network/WifiConnection.h"
#include "repository/SettingsKeys.h"

#include <QSet>
#include <QStringList>
#include <QUuid>

namespace dish::repository {

namespace {

// Must stay WifiConnection::idFor verbatim: the session manager files a
// satellite's pairing key under that id, so any divergence orphans the key.
QString idForServer(const models::DiscoveredServer& server) {
    return net::WifiConnection::idFor(server);
}

bool isBlank(const QString& s) { return s.trimmed().isEmpty(); }

// One-time in-place upgrade: older installs kept the remembered list under
// "wifi_list" and pairing keys under "wifi_shared_key/<id>". Copying them into
// the current namespaces is what spares those users a forced re-pair.
// Idempotent: skips once the new keys exist.
void migrateLegacyNamespaces(QSettings& settings) {
    if (!settings.contains(QLatin1String(keys::kSatelliteListKey)) &&
        settings.contains(QLatin1String(keys::kLegacyWifiListKey))) {
        settings.setValue(QLatin1String(keys::kSatelliteListKey),
                          settings.value(QLatin1String(keys::kLegacyWifiListKey)));
    }
    const QString legacyPrefix = QLatin1String(keys::kLegacySharedKeyPrefix);
    QStringList legacyKeys;
    for (const auto& key : settings.allKeys()) {
        if (key.startsWith(legacyPrefix)) { legacyKeys.append(key); }
    }
    for (const auto& legacyKey : legacyKeys) {
        const QString id = legacyKey.mid(legacyPrefix.size());
        const QString newKey = QLatin1String(keys::kSharedKeyPrefix) + id;
        if (!settings.contains(newKey)) { settings.setValue(newKey, settings.value(legacyKey)); }
    }
}

} // namespace

ConnectionStore::ConnectionStore(std::shared_ptr<QSettings> settings)
    : settings_(settings
                    ? std::move(settings)
                    : std::make_shared<QSettings>(QStringLiteral("Dish"), QStringLiteral("Dish"))),
      satellites_(nullptr), keys_(nullptr), pins_(nullptr) {
    migrateLegacyNamespaces(*settings_);
    satellites_ = std::make_unique<RememberedSatelliteRepository>(settings_);
    keys_ = std::make_unique<SatelliteSharedKeyRepository>(settings_);
    pins_ = std::make_unique<SatellitePinRepository>(settings_);
}

QString ConnectionStore::getOrCreateDeviceId() {
    // Non-const on purpose: const blocks the implicit move on the return below
    // and costs a QString copy on the common hit path.
    auto existing = settings_->value(QLatin1String(keys::kDeviceIdKey)).toString();
    if (!existing.isEmpty()) { return existing; }
    const auto fresh =
        QUuid::createUuid().toString(QUuid::WithoutBraces).remove(QLatin1Char('-')).toLower();
    settings_->setValue(QLatin1String(keys::kDeviceIdKey), fresh);
    return fresh;
}

QList<models::RememberedWifi> ConnectionStore::remembered() const {
    const auto all = satellites_->all();
    return QList<models::RememberedWifi>(all.begin(), all.end());
}

bool ConnectionStore::refreshKnownBox(const models::DiscoveredServer& server) {
    // A stable row is one that DID advertise a machineId; refreshing it in place
    // is what stops an ip:port ghost being minted beside it.
    for (const auto& row : satellites_->all()) {
        if (!isBlank(row.machineId) && row.ip == server.ip && row.udpPort == server.udpPort) {
            models::RememberedWifi refreshed = row;
            refreshed.name = server.name;
            refreshed.pairPort = server.pairPort;
            refreshed.httpPort = server.httpPort;
            if (refreshed != row) { satellites_->put(refreshed.id, refreshed); }
            return true;
        }
    }
    return false;
}

void ConnectionStore::collapseLegacyGhosts(const models::DiscoveredServer& server,
                                           const QString& id) {
    // Carry each ghost's pairing key forward to the stable id, but never over an
    // existing one — the stable row's own key wins.
    for (const auto& row : satellites_->all()) {
        if (isBlank(row.machineId) && row.ip == server.ip && row.udpPort == server.udpPort) {
            if (!keys_->get(id).has_value()) {
                if (const auto ghostKey = keys_->get(row.id)) { keys_->put(id, *ghostKey); }
            }
            keys_->remove(row.id);
            satellites_->remove(row.id);
        }
    }
}

void ConnectionStore::migratePinOnAddressChange(const std::optional<QString>& oldIp,
                                                const QString& newIp) {
    // A pin already trusted at the new address is NOT overwritten; the
    // old-address pin is ALWAYS dropped.
    if (!oldIp.has_value() || *oldIp == newIp) { return; }
    if (!pins_->pinnedFingerprint(newIp).has_value()) {
        if (const auto oldPin = pins_->pinnedFingerprint(*oldIp)) { pins_->pin(newIp, *oldPin); }
    }
    pins_->forget(*oldIp);
}

void ConnectionStore::rememberSatellite(const models::DiscoveredServer& server) {
    // A beacon without a machineId never mints a fresh row.
    if (isBlank(server.machineId) && refreshKnownBox(server)) { return; }

    const QString id = idForServer(server);
    if (!isBlank(server.machineId)) { collapseLegacyGhosts(server, id); }

    const auto existing = satellites_->get(id);
    const std::optional<QString> oldIp =
        existing.has_value() ? std::optional<QString>(existing->ip) : std::nullopt;
    migratePinOnAddressChange(oldIp, server.ip);

    models::RememberedWifi row;
    row.id = id;
    row.name = server.name;
    row.ip = server.ip;
    row.udpPort = server.udpPort;
    row.pairPort = server.pairPort;
    row.httpPort = server.httpPort;
    row.machineId = server.machineId;
    if (!existing.has_value() || *existing != row) { satellites_->put(id, row); }
}

void ConnectionStore::refreshFromDiscovery(const QList<models::DiscoveredServer>& discovered) {
    const auto rows = satellites_->all();
    QSet<QString> knownIds;
    for (const auto& r : rows) { knownIds.insert(r.id); }

    for (const auto& server : discovered) {
        // A beacon without a machineId never re-points a remembered row.
        if (isBlank(server.machineId)) { continue; }
        const QString id = idForServer(server);
        bool eligible = knownIds.contains(id);
        if (!eligible) {
            // Or a legacy ip:port row this stable server is the upgrade of.
            for (const auto& r : rows) {
                if (isBlank(r.machineId) && r.ip == server.ip && r.udpPort == server.udpPort) {
                    eligible = true;
                    break;
                }
            }
        }
        if (eligible) { rememberSatellite(server); }
    }
}

void ConnectionStore::forgetSatellite(const QString& id) {
    // Pin is keyed by IP; drop it via the row's IP before the row goes.
    if (const auto row = satellites_->get(id)) { pins_->forget(row->ip); }
    // Key before row: a crash mid-forget then leaves a re-pairable row rather
    // than an orphaned key.
    keys_->remove(id);
    satellites_->remove(id);
}

std::optional<QString> ConnectionStore::satelliteSharedKey(const QString& id) const {
    return keys_->get(id);
}

void ConnectionStore::setSatelliteSharedKey(const QString& id, const QString& keyHex) {
    keys_->put(id, keyHex);
}

void ConnectionStore::forgetSatelliteSharedKey(const QString& id) { keys_->remove(id); }

} // namespace dish::repository
