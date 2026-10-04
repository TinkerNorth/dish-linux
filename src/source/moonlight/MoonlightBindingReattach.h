// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Which standing Moonlight bindings to put back when the slot list moves. Pure,
// so the rule is tested without a bridge or a manager: a binding is reattached
// when its pad is on the slot list, it drives nothing yet, no satellite binding
// holds the slot (the two tables are exclusive, and the satellite one wins as
// it does on the slot list), and it has not been tried since the pad appeared.
// That last gate is what keeps a bind the manager refuses (a full host) from
// being asked again on every rebuild the attempt itself causes.

#pragma once

#include "repository/MoonlightHostRepository.h"

#include <QSet>
#include <QString>

#include <vector>

namespace dish::source::moon {

inline std::vector<repository::MoonlightBinding>
bindingsToReattach(const std::vector<repository::MoonlightBinding>& standing,
                   const QSet<QString>& presentSlotIds, const QSet<QString>& moonlightBoundSlotIds,
                   const QSet<QString>& satelliteBoundSlotIds, const QSet<QString>& triedSlotIds) {
    std::vector<repository::MoonlightBinding> out;
    for (const auto& binding : standing) {
        const bool padIsHere = presentSlotIds.contains(binding.slotId);
        const bool drivesNothing = !moonlightBoundSlotIds.contains(binding.slotId);
        const bool slotIsFree = !satelliteBoundSlotIds.contains(binding.slotId);
        const bool notTriedYet = !triedSlotIds.contains(binding.slotId);
        if (padIsHere && drivesNothing && slotIsFree && notTriedYet) { out.push_back(binding); }
    }
    return out;
}

} // namespace dish::source::moon
