// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "source/store/MicMuteStore.h"

namespace dish::source {

bool MicMuteStore::isMuted(const std::string& slotId) const {
    const auto& snapshot = state().value();
    const auto it = snapshot.find(slotId);
    if (it == snapshot.end()) { return kDefaultMuted; }
    return it->second;
}

void MicMuteStore::setMuted(const std::string& slotId, bool muted) {
    setState([&](const MicMuteMap& current) {
        const auto it = current.find(slotId);
        if (it != current.end() && it->second == muted) { return current; }
        MicMuteMap next = current;
        next[slotId] = muted;
        return next;
    });
}

bool MicMuteStore::toggle(const std::string& slotId) {
    const bool next = !isMuted(slotId);
    setMuted(slotId, next);
    return next;
}

void MicMuteStore::retainOnly(const std::set<std::string>& present) {
    setState([&](const MicMuteMap& current) {
        MicMuteMap next;
        for (const auto& [slotId, muted] : current) {
            if (present.find(slotId) != present.end()) { next.emplace(slotId, muted); }
        }
        return next;
    });
}

} // namespace dish::source
