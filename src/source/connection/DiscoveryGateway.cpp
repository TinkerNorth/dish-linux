// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "DiscoveryGateway.h"

#include "LANDiscovery.h"
#include "MdnsDiscovery.h"
#include "core/model/IdentityKey.h"

#include <QHash>

#include <QtConcurrent/QtConcurrent>

#include <algorithm>

namespace dish::net {

namespace {

QString stableKeyOf(const models::DiscoveredServer& s) {
    return QString::fromStdString(
        model::stableKey(s.machineId.toStdString(), s.ip.toStdString(), s.udpPort));
}

} // namespace

QList<models::DiscoveredServer>
DiscoveryGateway::mergeDiscovered(const QList<models::DiscoveredServer>& broadcast,
                                  const QList<models::DiscoveredServer>& mdns) {
    QList<models::DiscoveredServer> ordered; // preserves first-seen order
    QHash<QString, int> indexByKey;          // stable key → index in `ordered`

    for (auto server : broadcast) {
        server.source = models::DiscoverySource::Broadcast;
        const QString key = stableKeyOf(server);
        const auto it = indexByKey.constFind(key);
        if (it != indexByKey.constEnd()) {
            ordered[it.value()] = server; // last write wins on the key
        } else {
            indexByKey.insert(key, static_cast<int>(ordered.size()));
            ordered.append(server);
        }
    }
    for (auto server : mdns) {
        const QString key = stableKeyOf(server);
        const auto it = indexByKey.constFind(key);
        const models::DiscoverySource source = (it != indexByKey.constEnd())
                                                   ? models::DiscoverySource::Both
                                                   : models::DiscoverySource::Mdns;
        server.source = source;
        if (it != indexByKey.constEnd()) {
            ordered[it.value()] = server;
        } else {
            indexByKey.insert(key, static_cast<int>(ordered.size()));
            ordered.append(server);
        }
    }

    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const models::DiscoveredServer& a, const models::DiscoveredServer& b) {
                         return a.name < b.name;
                     });
    return ordered;
}

QString DiscoveryGateway::pinId(const QString& satelliteId, const QString& ip) {
    return satelliteId.isEmpty() ? ip : satelliteId;
}

QList<models::DiscoveredServer> DiscoveryGateway::discover() {
    auto mdnsFuture = QtConcurrent::run([] { return MdnsDiscovery::discover(); });
    const QList<models::DiscoveredServer> beacon = LANDiscovery::discover();
    const QList<models::DiscoveredServer> mdns = mdnsFuture.result();
    return mergeDiscovered(beacon, mdns);
}

} // namespace dish::net
