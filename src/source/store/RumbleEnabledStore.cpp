// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "source/store/RumbleEnabledStore.h"

namespace dish::source {

RumbleEnabledMap RumbleEnabledStore::hydrate(repository::RumblePreferenceRepository* repo) {
    RumbleEnabledMap out;
    for (const auto& pref : repo->all()) { out[pref.slotId.toStdString()] = pref.enabled; }
    return out;
}

RumbleEnabledStore::RumbleEnabledStore(repository::RumblePreferenceRepository* repo)
    : arch::StateSource<RumbleEnabledMap>(hydrate(repo)), repo_(repo) {}

bool RumbleEnabledStore::isEnabled(const std::string& slotId) const {
    const auto switches = state().value();
    const auto it = switches.find(slotId);
    if (it == switches.end()) { return kDefaultEnabled; }
    return it->second;
}

void RumbleEnabledStore::setEnabled(const std::string& slotId, bool enabled) {
    repo_->put(repository::RumblePreference{QString::fromStdString(slotId), enabled});
    setState([&](const RumbleEnabledMap& current) {
        RumbleEnabledMap next = current;
        next[slotId] = enabled;
        return next;
    });
}

} // namespace dish::source
