// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Which connections a slot's bind picker offers, and which catalog controller
// types are pickable for it.

#pragma once

#include "Models/Models.h"
#include "core/model/Protocol.h"
#include "core/reducer/EmulateSeed.h"

#include <QList>
#include <QString>

#include <optional>

namespace dish::reducer {

// Only a link that is usable right now: everything else would be a dead pick.
inline bool isAvailableForPicker(models::LinkState live) {
    switch (live) {
    case models::LinkState::Connected:
    case models::LinkState::Unstable:
        return true;
    case models::LinkState::Connecting:
    case models::LinkState::Ready:
    case models::LinkState::Found:
    case models::LinkState::Saved:
    case models::LinkState::Stale:
        return false;
    }
    return false;
}

// The currently-bound connection is held over even when offline, so a flaky host
// does not vanish from under the user mid-session; it leaves the list only once
// they unbind or it recovers. Input order is the row order and is preserved.
inline QList<models::ConnectionSummary>
connectionsVisibleInPicker(const QList<models::ConnectionSummary>& all,
                           const std::optional<QString>& boundConnectionId) {
    QList<models::ConnectionSummary> visible;
    visible.reserve(all.size());
    for (const auto& c : all) {
        const bool bound = boundConnectionId.has_value() && c.id == *boundConnectionId;
        if (isAvailableForPicker(c.live) || bound) { visible.push_back(c); }
    }
    return visible;
}

// Per the contract an unknown controller-type slug still renders, so the only
// gate is that the row carries a display name. A type newer than the app renders
// fine from the server-provided name.
inline bool isTypeOfferable(const models::CatalogTypeDto& type) {
    return !type.name.isEmpty() || !type.shortName.isEmpty();
}

// The ladder: user override, then the type whose `emulates` hint matches the
// detected pad, then the catalog's first offered row, then Xbox when no catalog
// is cached. An empty pad identity or a hint-less catalog matches nothing, so the
// ladder degrades to first-offered.
inline int seedControllerType(std::optional<int> userOverride,
                              const std::optional<models::CatalogDto>& catalog,
                              const QString& sdlTypeSlug, const QString& vidPid) {
    if (userOverride) { return *userOverride; }
    if (catalog) {
        QList<models::CatalogTypeDto> offered;
        offered.reserve(catalog->controllerTypes.size());
        for (const auto& t : catalog->controllerTypes) {
            if (isTypeOfferable(t)) { offered.push_back(t); }
        }
        if (const auto match = seedFromEmulates(offered, sdlTypeSlug, vidPid)) { return *match; }
        if (!offered.isEmpty()) { return offered.front().id; }
    }
    return proto::kControllerTypeXbox;
}

// For callers with no detected pad in hand: hint matching short-circuits.
inline int seedControllerType(std::optional<int> userOverride,
                              const std::optional<models::CatalogDto>& catalog) {
    return seedControllerType(userOverride, catalog, QString(), QString());
}

// An unknown feature slug, one a newer server invented, is silently not offered
// rather than an error. `known` is supplied by the caller so this stays a pure
// data decision; catalog::knownFeatureSlugs owns the list.
inline bool isFeatureOffered(const models::CatalogTypeDto& type, const QString& featureSlug,
                             const QList<QString>& known) {
    if (!known.contains(featureSlug)) { return false; }
    const auto it = type.features.constFind(featureSlug);
    return it != type.features.constEnd() && it->supported;
}

} // namespace dish::reducer
