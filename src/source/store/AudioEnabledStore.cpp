// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "source/store/AudioEnabledStore.h"

namespace dish::source {

AudioEnabledMap AudioEnabledStore::hydrate(repository::AudioPreferenceRepository* repo) {
    AudioEnabledMap out;
    if (repo == nullptr) { return out; }
    for (const auto& pref : repo->all()) { out[pref.slotId.toStdString()] = pref.enabled; }
    return out;
}

AudioEnabledStore::AudioEnabledStore(repository::AudioPreferenceRepository* repo,
                                     bool defaultEnabled)
    : arch::StateSource<AudioEnabledMap>(hydrate(repo)), repo_(repo),
      defaultEnabled_(defaultEnabled) {}

bool AudioEnabledStore::isEnabled(const std::string& slotId) const {
    const auto& snapshot = state().value();
    const auto it = snapshot.find(slotId);
    if (it == snapshot.end()) { return defaultEnabled_; }
    return it->second;
}

void AudioEnabledStore::setEnabled(const std::string& slotId, bool enabled) {
    if (repo_ != nullptr) {
        repo_->put(repository::AudioPreference{QString::fromStdString(slotId), enabled});
    }
    setState([&](const AudioEnabledMap& current) {
        AudioEnabledMap next = current;
        next[slotId] = enabled;
        return next;
    });
}

void AudioEnabledStore::forget(const std::string& slotId) {
    if (repo_ != nullptr) { repo_->remove(QString::fromStdString(slotId)); }
    setState([&](const AudioEnabledMap& current) {
        if (current.find(slotId) == current.end()) { return current; }
        AudioEnabledMap next = current;
        next.erase(slotId);
        return next;
    });
}

} // namespace dish::source
