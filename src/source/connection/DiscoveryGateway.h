// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// DiscoveryGateway — runs the broadcast beacon (LANDiscovery) and the mDNS
// browse (MdnsDiscovery) and fuses them into one source-tagged list.
//
// De-dupe is by the STABLE key (machineId-preferring), not by address: a box
// that answers the beacon on one port and mDNS with a different SRV port under
// the same machineId is ONE satellite, and an address-keyed merge would show it
// twice.

#pragma once

#include "Models/Models.h"

#include <QList>
#include <QString>

namespace dish::net {

class DiscoveryGateway {
  public:
    // Stable key is machineId else ip:udpPort; broadcast-only → Broadcast,
    // mdns-only → Mdns, both → Both. Sorted by name.
    static QList<models::DiscoveredServer>
    mergeDiscovered(const QList<models::DiscoveredServer>& broadcast,
                    const QList<models::DiscoveredServer>& mdns);

    // The id a cert pin is filed under: the explicit satellite id when present,
    // else the observed ip.
    static QString pinId(const QString& satelliteId, const QString& ip);

    // Run both discovery paths and return the merged, source-tagged list.
    // Blocking; call from a background thread.
    static QList<models::DiscoveredServer> discover();
};

} // namespace dish::net
