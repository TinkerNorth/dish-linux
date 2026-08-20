// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Derives the controller types the "Emulate" picker offers from a cached
// catalog. The per-slot remembered selection is deliberately NOT derived here —
// it lives in ControllerTypeStore and is read at the picker call site.

#pragma once

#include "Models/Models.h"
#include "architecture/Composer.h"
#include "architecture/Observable.h"

#include <QList>
#include <QString>

#include <cstdint>

namespace dish::composer {

// One row the Emulate picker renders. The display strings are server-provided
// and already localized. `known` means the client has bundled art for the slug;
// an unknown slug still renders from the server strings (forward-compat).
struct PickableType {
    int type = 0;
    QString slug;
    QString name;
    QString shortName;
    QString description;
    bool known = false;

    bool operator==(const PickableType& o) const {
        return type == o.type && slug == o.slug && name == o.name && shortName == o.shortName &&
               description == o.description && known == o.known;
    }
    bool operator!=(const PickableType& o) const { return !(*this == o); }
};

// CatalogDto has no operator==, so it cannot be an Observable value directly.
// This wrapper supplies one keyed on the ETag — which IS the catalog's content
// identity ("<serverVersion>+<locale>") — so distinct-until-changed suppresses
// no-op re-emits and a 304 revalidate does not retrigger the picker list.
struct CatalogSnapshot {
    models::CatalogDto catalog;

    bool operator==(const CatalogSnapshot& o) const {
        return catalog.etag == o.catalog.etag && catalog.reachable == o.catalog.reachable &&
               catalog.serverVersion == o.catalog.serverVersion &&
               catalog.locale == o.catalog.locale;
    }
    bool operator!=(const CatalogSnapshot& o) const { return !(*this == o); }
};

// The slugs this client ships bundled art for. Must stay in lockstep with the
// protocol-1 controller types.
QList<QString> knownTypeSlugs();

// Every catalog type that carries a display name is offered, even one newer than
// the app; a nameless row is dropped. Order follows the catalog (server-curated).
QList<PickableType> offerableTypes(const models::CatalogDto& catalog);

class CatalogComposer : public arch::Composer<QList<PickableType>, CatalogSnapshot> {
  public:
    explicit CatalogComposer(const arch::Observable<CatalogSnapshot>& catalog)
        : arch::Composer<QList<PickableType>, CatalogSnapshot>(
              catalog, [](const CatalogSnapshot& c) { return offerableTypes(c.catalog); }) {}
};

} // namespace dish::composer
