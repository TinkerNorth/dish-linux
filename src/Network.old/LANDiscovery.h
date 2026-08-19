// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#pragma once

#include "Models/Models.h"

#include <QList>
#include <QString>

#include <optional>

namespace dish::net {

// Listens on UDP :9879 for Satellite beacon broadcasts. Mirrors
// dish-mac/Network/LANDiscovery.swift and satellite_jni.cpp::discoverServers.
class LANDiscovery {
  public:
    static constexpr int kDefaultPort = 9879;
    static constexpr int kDefaultTimeoutMs = 4000;

    // Blocking. Call from a background thread. Returns every unique server
    // heard within `timeoutMs`. Each recv has a 300 ms timeout so quiet
    // networks don't hang the caller past the deadline.
    static QList<models::DiscoveredServer> discover(int port = kDefaultPort,
                                                    int timeoutMs = kDefaultTimeoutMs);

    // Lenient parse — public so unit tests can exercise the JSON path without
    // opening a UDP socket. Returns std::nullopt for malformed beacons / the
    // wrong service or any beacon with an empty `name`.
    static std::optional<models::DiscoveredServer> parseBeacon(const QString& json,
                                                               const QString& observedIp);
};

} // namespace dish::net
