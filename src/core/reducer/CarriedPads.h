// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// "Which pads ride this connection, and as what?" — the manifest a Forget
// confirm has to show before it drops them.
//
// A pure join over the slot list, because the alternative the UI reaches for is
// instantiating an invisible delegate per slot and reading its properties back
// out. That re-derives domain state in the view layer, and it is invisible to
// static analysis: `Repeater::itemAt` is typed `QQuickItem`, so every field read
// off it is unresolvable.
//
// The parameter is `slotList`, never `slots`: Qt defines `slots` as a macro, so
// a parameter of that name does not compile in any TU that has seen a Qt
// header.

#pragma once

#include "Models/Models.h"

#include <QList>
#include <QString>

namespace dish::reducer {

struct CarriedPad {
    QString name;
    QString emulateName; // "" when the catalog offers no name for the type
};

// Empty for an empty id, so a caller with no selection needs no special case.
// Order follows the slot list, which is already the order the pad rows render
// in, so the manifest reads in the same sequence the user just looked at.
inline QList<CarriedPad> carriedPads(const QList<models::ControllerSlot>& slotList,
                                     const QString& connectionId) {
    QList<CarriedPad> out;
    if (connectionId.isEmpty()) { return out; }
    for (const auto& slot : slotList) {
        if (slot.boundConnectionId.has_value() && *slot.boundConnectionId == connectionId) {
            out.append(CarriedPad{slot.name, slot.emulateName});
        }
    }
    return out;
}

// The 1-based position of `slotId` among the slots riding `connectionId`, or 0
// when it rides none. Same join, same ordering, so "slot 2 of 4" agrees with
// the manifest above it.
inline int slotOrdinalOnConnection(const QList<models::ControllerSlot>& slotList,
                                   const QString& slotId, const QString& connectionId) {
    if (connectionId.isEmpty()) { return 0; }
    int ordinal = 0;
    for (const auto& slot : slotList) {
        if (!slot.boundConnectionId.has_value() || *slot.boundConnectionId != connectionId) {
            continue;
        }
        ++ordinal;
        if (slot.id == slotId) { return ordinal; }
    }
    return 0;
}

} // namespace dish::reducer
