// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Offline per-slug capability sets for the controller types this client ships
// art and translations for: the fallback before any server catalog is fetched,
// and the feature source LegacyCatalogTranslator rebuilds legacy catalogs from.
// Unknown slugs return nullopt so a richer remote type is never masked by a stale
// bundled guess. Qt containers appear here because the CatalogTypeDto vocabulary
// is the data being produced; core/catalog uses Qt only where the DTOs force it.

#pragma once

#include "core/model/Protocol.h"

#include <QString>
#include <QStringList>

#include <optional>

namespace dish::catalog {

// The bundled type slugs, in wire-id order (proto::kControllerType*).
inline const QString kSlugXbox360 = QStringLiteral("xbox360");
inline const QString kSlugDs4 = QStringLiteral("ds4");
inline const QString kSlugDualSense = QStringLiteral("dualsense");
inline const QString kSlugSwitchPro = QStringLiteral("switchpro");

// Catalog feature-slug vocabulary (protocol constants, never localized).
inline const QString kFeatureAnalogTriggers = QStringLiteral("analogTriggers");
inline const QString kFeatureRumble = QStringLiteral("rumble");
inline const QString kFeatureMotion = QStringLiteral("motion");
inline const QString kFeatureLightbar = QStringLiteral("lightbar");
inline const QString kFeatureTouchpad = QStringLiteral("touchpad");

// The `known` whitelist reducer::isFeatureOffered gates on, owned here so every
// caller passes the same vocabulary instead of re-listing it.
inline QStringList knownFeatureSlugs() {
    return {kFeatureRumble, kFeatureAnalogTriggers, kFeatureMotion, kFeatureLightbar,
            kFeatureTouchpad};
}

// Order is fixed (triggers, rumble, extras) so the list is ==-comparable in tests
// and downstream snapshots.
inline std::optional<QStringList> typeFeatureSlugs(const QString& slug) {
    const QStringList base{kFeatureAnalogTriggers, kFeatureRumble};
    if (slug == kSlugXbox360) { return base; }
    if (slug == kSlugDs4 || slug == kSlugDualSense) {
        return base + QStringList{kFeatureMotion, kFeatureTouchpad, kFeatureLightbar};
    }
    if (slug == kSlugSwitchPro) { return base + QStringList{kFeatureMotion}; }
    return std::nullopt;
}

// An id outside the bundled range degrades to the xbox360 set, the least-capable
// baseline, rather than claiming features an unknown type may not have.
inline QStringList typeFeatureSlugsById(int typeId) {
    switch (typeId) {
    case proto::kControllerTypePlayStation:
        return *typeFeatureSlugs(kSlugDs4);
    case proto::kControllerTypeDualSense:
        return *typeFeatureSlugs(kSlugDualSense);
    case proto::kControllerTypeSwitchPro:
        return *typeFeatureSlugs(kSlugSwitchPro);
    default:
        return *typeFeatureSlugs(kSlugXbox360);
    }
}

} // namespace dish::catalog
