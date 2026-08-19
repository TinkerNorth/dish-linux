// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#pragma once

#include "Models/Models.h"

#include <QList>
#include <QString>

#include <optional>

namespace dish::net {

// Listens on UDP :9879 for satellite beacon broadcasts.
class LANDiscovery {
  public:
    static constexpr int kDefaultPort = 9879;
    static constexpr int kDefaultTimeoutMs = 4000;

    // Blocking; call from a background thread. Each recv has its own short
    // timeout so a quiet network can't hang the caller past `timeoutMs`.
    static QList<models::DiscoveredServer> discover(int port = kDefaultPort,
                                                    int timeoutMs = kDefaultTimeoutMs);

    // Public so tests can exercise the JSON path without opening a UDP socket.
    // nullopt for a malformed beacon, the wrong service, or an empty `name`.
    static std::optional<models::DiscoveredServer> parseBeacon(const QString& json,
                                                               const QString& observedIp);
};

} // namespace dish::net
