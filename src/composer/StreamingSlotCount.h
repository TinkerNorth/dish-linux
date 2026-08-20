// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// How many slots are actively streaming to a satellite — the upstream count the
// WakeStateComposer folds into a WakeState. Lives in composer/ rather than core/
// only because it speaks the Qt vocabulary of the connection layer.

#pragma once

#include "Models/Models.h"

#include <QHash>
#include <QString>

namespace dish::composer {

// Connected is the only state actually exchanging packets; every other state is
// paired or pending, so it must not keep the display awake.
inline int streamingSlotCount(const QHash<QString, QString>& bindings,
                              const QHash<QString, models::LinkState>& connectionStates) {
    int count = 0;
    for (auto it = bindings.begin(); it != bindings.end(); ++it) {
        if (connectionStates.value(it.value(), models::LinkState::Saved) ==
            models::LinkState::Connected) {
            ++count;
        }
    }
    return count;
}

} // namespace dish::composer
