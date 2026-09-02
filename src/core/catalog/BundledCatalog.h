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
// Controller audio (protocol 2). Deliberately NOT in knownFeatureSlugs():
// that list is the protocol-1 caps-gate vocabulary (reducer::allowedCapsForType),
// and the audio caps follow the trigger-effects precedent of passing through it
// untouched. The capability solver's type layer reads these two directly.
inline const QString kFeatureMic = QStringLiteral("mic");
inline const QString kFeatureSpeaker = QStringLiteral("speaker");

// The `known` whitelist reducer::isFeatureOffered gates on, owned here so every
// caller passes the same vocabulary instead of re-listing it.
inline QStringList knownFeatureSlugs() {
    return {kFeatureRumble, kFeatureAnalogTriggers, kFeatureMotion, kFeatureLightbar,
            kFeatureTouchpad};
}

// The whitelist for the solver's audio type-layer reads, so the same
// isFeatureOffered gate serves them without widening the protocol-1 list.
inline QStringList audioFeatureSlugs() { return {kFeatureMic, kFeatureSpeaker}; }

// Order is fixed (triggers, rumble, extras) so the list is ==-comparable in tests
// and downstream snapshots.
//
// Audio rides the two Sony types only: they are the pads that carry real
// speaker and microphone endpoints, so they are the only identities a host can
// materialize with any. Offering them here cannot outrun the host, which gates
// audio on its own runtime controllerAudio switch, and a satellite old enough
// to serve no catalog reports no switch at all (mirrors dish-android's
// BundledCatalog).
inline std::optional<QStringList> typeFeatureSlugs(const QString& slug) {
    QStringList base{kFeatureAnalogTriggers, kFeatureRumble};
    if (slug == kSlugXbox360) { return base; }
    if (slug == kSlugDs4 || slug == kSlugDualSense) {
        return base + QStringList{kFeatureMotion, kFeatureTouchpad, kFeatureLightbar, kFeatureMic,
                                  kFeatureSpeaker};
    }
    if (slug == kSlugSwitchPro) { return base + QStringList{kFeatureMotion}; }
    return std::nullopt;
}

// An id outside the bundled range degrades to the xbox360 set, the least-capable
// baseline, rather than claiming features an unknown type may not have. The
// same floor covers a slug that stops resolving: value_or, not a dereference,
// so a future edit to typeFeatureSlugs degrades instead of being undefined.
inline QStringList typeFeatureSlugsById(int typeId) {
    QStringList floor = typeFeatureSlugs(kSlugXbox360).value_or(QStringList{});
    switch (typeId) {
    case proto::kControllerTypePlayStation:
        return typeFeatureSlugs(kSlugDs4).value_or(floor);
    case proto::kControllerTypeDualSense:
        return typeFeatureSlugs(kSlugDualSense).value_or(floor);
    case proto::kControllerTypeSwitchPro:
        return typeFeatureSlugs(kSlugSwitchPro).value_or(floor);
    default:
        return floor;
    }
}

} // namespace dish::catalog
