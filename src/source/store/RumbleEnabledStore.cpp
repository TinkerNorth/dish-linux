// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "source/store/RumbleEnabledStore.h"

namespace dish::source {

namespace {

bool switchFor(const RumbleEnabledMap& switches, const QString& slotId) {
    const auto it = switches.find(slotId);
    if (it == switches.end()) { return RumbleEnabledStore::kDefaultEnabled; }
    return it->second;
}

} // namespace

RumbleEnabledMap RumbleEnabledStore::hydrate(repository::RumblePreferenceRepository* repo) {
    RumbleEnabledMap out;
    for (const auto& pref : repo->all()) { out[pref.slotId] = pref.enabled; }
    return out;
}

RumbleEnabledStore::RumbleEnabledStore(repository::RumblePreferenceRepository* repo)
    : arch::StateSource<RumbleEnabledMap>(hydrate(repo)), repo_(repo) {}

bool RumbleEnabledStore::isEnabled(const QString& slotId) const {
    return state().read(
        [&slotId](const RumbleEnabledMap& switches) { return switchFor(switches, slotId); });
}

void RumbleEnabledStore::setEnabled(const QString& slotId, bool enabled) {
    repo_->put(repository::RumblePreference{slotId, enabled});
    setState([&](const RumbleEnabledMap& current) {
        RumbleEnabledMap next = current;
        next[slotId] = enabled;
        return next;
    });
}

void RumbleEnabledStore::forget(const QString& slotId) {
    repo_->remove(slotId);
    setState([&](const RumbleEnabledMap& current) {
        RumbleEnabledMap next = current;
        next.erase(slotId);
        return next;
    });
}

} // namespace dish::source
