// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Which standing Moonlight bindings to put back when the slot list moves. Pure, so
// the rule is tested without a bridge or a manager. The tried set exists because a
// bind the manager refuses rebuilds the list, which would ask again on every turn.

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
