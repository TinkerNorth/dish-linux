// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Which standing Moonlight bindings to put back when the slot list moves, and under which slot.
// Pure, so the rule is tested without a bridge or a manager. A record that carries the pad's
// identity follows the pad to whatever slot it holds now; a record written before identities
// existed follows its slot id, as it always did. The tried set exists because a bind the manager
// refuses rebuilds the list, which would ask again on every turn.

#pragma once

#include "repository/MoonlightHostRepository.h"

#include <QSet>
#include <QString>

#include <vector>

namespace dish::source::moon {

struct PresentPad {
    QString slotId;
    QString identity;
};

struct Reattach {
    repository::MoonlightBinding binding;
    // Where the pad is now, and what it is: the record is rewritten with both when they differ.
    QString slotId;
    QString identity;
};

inline std::vector<Reattach>
bindingsToReattach(const std::vector<repository::MoonlightBinding>& standing,
                   const std::vector<PresentPad>& present,
                   const QSet<QString>& moonlightBoundSlotIds,
                   const QSet<QString>& satelliteBoundSlotIds, const QSet<QString>& triedSlotIds) {
    std::vector<Reattach> out;
    QSet<QString> claimed;
    for (const auto& binding : standing) {
        const PresentPad* pad = nullptr;
        for (const auto& candidate : present) {
            if (claimed.contains(candidate.slotId)) { continue; }
            const bool byIdentity =
                !binding.padIdentity.isEmpty() && candidate.identity == binding.padIdentity;
            const bool bySlot = binding.padIdentity.isEmpty() && candidate.slotId == binding.slotId;
            if (byIdentity || bySlot) {
                pad = &candidate;
                break;
            }
        }
        if (pad == nullptr) { continue; }
        claimed.insert(pad->slotId);
        const bool drivesNothing = !moonlightBoundSlotIds.contains(pad->slotId);
        const bool slotIsFree = !satelliteBoundSlotIds.contains(pad->slotId);
        const bool notTriedYet = !triedSlotIds.contains(pad->slotId);
        if (drivesNothing && slotIsFree && notTriedYet) {
            out.push_back({binding, pad->slotId, pad->identity});
        }
    }
    return out;
}

} // namespace dish::source::moon
