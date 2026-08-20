// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Which discovered satellites the Connections page's FOUND card offers.

#pragma once

#include "Models/Models.h"

#include <QList>
#include <QSet>
#include <QString>

namespace dish::reducer {

// A satellite renders in exactly one of the two Connections lists: anything with
// a connections row renders there, and FOUND offers only the rest. Keying on the
// row-id set rather than remembered ids alone keeps a live-but-not-yet-remembered
// session (mid-pair, before the first PUT persists it) from showing twice. Scan
// order is the FOUND row order, so it is preserved.
inline QList<models::DiscoveredServer>
serversVisibleInFound(const QList<models::DiscoveredServer>& discovered,
                      const QSet<QString>& connectionRowIds) {
    QList<models::DiscoveredServer> visible;
    visible.reserve(discovered.size());
    for (const auto& s : discovered) {
        if (!connectionRowIds.contains(s.id())) { visible.push_back(s); }
    }
    return visible;
}

} // namespace dish::reducer
