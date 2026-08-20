// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Every catalog-version to canonical mapping, so the rest of the app stays
// version-agnostic. The contract allows a client to substitute its own known
// representation for a recognized legacy version, so a legacy body's
// controllerTypes are not trusted: only its version is read, and everything else
// (locale, server version, host features, etag/status/reachable) passes through.
// Applied at the repository boundary on both 200-fill and stale-serve so cache
// and callers see one normalized shape. The substituted rows carry display
// strings because the offerable-type projection drops nameless rows, which would
// otherwise blank the picker for a legacy satellite; the slugs are `known`, so
// the UI still swaps bundled translations over these English fallbacks.

#pragma once

#include "Models/Models.h"
#include "core/catalog/BundledCatalog.h"
#include "core/model/Protocol.h"

#include <QHash>
#include <QString>
#include <QStringList>

namespace dish::catalog {

// The schema version this client is written against. Bump only together with a
// new legacy*() representation of the version being left behind.
inline constexpr int kCatalogVersionCurrent = 2;

// v1 is the sole legacy schema: xbox360 + ds4 only, no emulates.
inline constexpr int kCatalogVersionLegacyV1 = 1;

namespace detail {

// Only controllerTypes are hardcoded; their capability sets reuse
// BundledCatalog rather than re-listing feature slugs here.
inline QHash<QString, models::CatalogFeatureDto> legacyFeatures(const QString& slug) {
    QHash<QString, models::CatalogFeatureDto> out;
    const auto slugs = typeFeatureSlugs(slug);
    if (!slugs) { return out; }
    for (const auto& featureSlug : *slugs) {
        models::CatalogFeatureDto feature;
        feature.supported = true;
        if (featureSlug == kFeatureTouchpad) {
            // The resolver gates the touchpad on the "ds4" mode slug, so the
            // substitution has to advertise it.
            const auto ds4 = proto::touchpadModeName(proto::kTouchpadModeDs4);
            feature.modes =
                QStringList{QString::fromUtf8(ds4.data(), static_cast<qsizetype>(ds4.size()))};
        }
        out.insert(featureSlug, feature);
    }
    return out;
}

inline models::CatalogTypeDto legacyType(int id, const QString& slug, const QString& name,
                                         const QString& shortName) {
    models::CatalogTypeDto type;
    type.id = id;
    type.slug = slug;
    type.name = name;
    type.shortName = shortName;
    type.features = legacyFeatures(slug);
    return type;
}

inline models::CatalogDto legacyV1(const models::CatalogDto& fetched) {
    models::CatalogDto out = fetched;
    out.catalogVersion = kCatalogVersionLegacyV1;
    out.controllerTypes = {
        legacyType(proto::kControllerTypeXbox, kSlugXbox360, QStringLiteral("Xbox 360 Controller"),
                   QStringLiteral("Xbox")),
        legacyType(proto::kControllerTypePlayStation, kSlugDs4, QStringLiteral("DualShock 4"),
                   QStringLiteral("PlayStation")),
    };
    return out;
}

} // namespace detail

// Deliberately >= rather than per-version cases: a newer-than-current catalog is
// additive within protocolVersion 1 and must pass through for forward-compat.
// Idempotent, so re-applying on stale-serve is safe by construction.
inline models::CatalogDto normalizeCatalog(const models::CatalogDto& fetched) {
    if (fetched.catalogVersion >= kCatalogVersionCurrent) { return fetched; }
    return detail::legacyV1(fetched);
}

} // namespace dish::catalog
