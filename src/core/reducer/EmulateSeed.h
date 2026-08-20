// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Which offered virtual type is the natural default for a detected physical pad,
// resolved from the catalog's optional per-type `emulates` hints. The mapping
// policy lives on the server so new hardware needs no client release; this only
// matches the pad's identity against it. With no hints the caller degrades to
// first-offered, so an interim or legacy catalog behaves as it always did.

#pragma once

#include "Models/Models.h"

#include <QChar>
#include <QList>
#include <QString>

#include <optional>

namespace dish::reducer {

// The catalog's `usb` identities are lowercase zero-padded hex "vid:pid". A 0/0
// identity means SDL could not read the descriptor, so it yields empty and never
// matches rather than colliding with a real key.
inline QString vidPidKey(int vendorId, int productId) {
    if (vendorId == 0 && productId == 0) { return {}; }
    return QStringLiteral("%1:%2")
        .arg(vendorId & 0xFFFF, 4, 16, QLatin1Char('0'))
        .arg(productId & 0xFFFF, 4, 16, QLatin1Char('0'));
}

// Both sides are lowercase protocol constants, so the compares are verbatim.
// Empty detected fields match nothing, so an unknown pad cannot accidentally
// equal a hint whose own field is absent.
inline bool emulatesMatches(const models::CatalogEmulatesDto& hint, const QString& sdlTypeSlug,
                            const QString& vidPid) {
    if (!sdlTypeSlug.isEmpty() && hint.sdlType == sdlTypeSlug) { return true; }
    return !vidPid.isEmpty() && hint.usb.contains(vidPid);
}

// The caller pre-filters with isTypeOfferable; per the contract, hints ride only
// offered types. First match in catalog order wins, so a pad matching two hints
// gets the server's curated preference.
inline std::optional<int> seedFromEmulates(const QList<models::CatalogTypeDto>& offeredTypes,
                                           const QString& sdlTypeSlug, const QString& vidPid) {
    for (const auto& type : offeredTypes) {
        if (!type.emulates) { continue; }
        if (emulatesMatches(*type.emulates, sdlTypeSlug, vidPid)) { return type.id; }
    }
    return std::nullopt;
}

} // namespace dish::reducer
