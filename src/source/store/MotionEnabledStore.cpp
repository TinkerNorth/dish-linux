// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "source/store/MotionEnabledStore.h"

namespace dish::source {

MotionEnabledMap MotionEnabledStore::hydrate(repository::MotionPreferenceRepository* repo) {
    MotionEnabledMap out;
    if (repo == nullptr) { return out; }
    for (const auto& pref : repo->all()) { out[pref.slotId.toStdString()] = pref.enabled; }
    return out;
}

MotionEnabledStore::MotionEnabledStore(repository::MotionPreferenceRepository* repo)
    : arch::StateSource<MotionEnabledMap>(hydrate(repo)), repo_(repo) {}

bool MotionEnabledStore::isEnabled(const std::string& slotId) const {
    const auto& snapshot = state().value();
    const auto it = snapshot.find(slotId);
    if (it == snapshot.end()) { return kDefaultEnabled; }
    return it->second;
}

void MotionEnabledStore::setEnabled(const std::string& slotId, bool enabled) {
    if (repo_ != nullptr) {
        repo_->put(repository::MotionPreference{QString::fromStdString(slotId), enabled});
    }
    setState([&](const MotionEnabledMap& current) {
        MotionEnabledMap next = current;
        next[slotId] = enabled;
        return next;
    });
}

void MotionEnabledStore::forget(const std::string& slotId) {
    if (repo_ != nullptr) { repo_->remove(QString::fromStdString(slotId)); }
    setState([&](const MotionEnabledMap& current) {
        if (current.find(slotId) == current.end()) { return current; }
        MotionEnabledMap next = current;
        next.erase(slotId);
        return next;
    });
}

} // namespace dish::source
