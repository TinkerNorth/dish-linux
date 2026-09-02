// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// AudioEnabledStore — a StateSource over one per-slot controller-audio toggle:
// slotId -> bool, bridging a durable AudioPreferenceRepository with a reactive
// in-memory Observable<map>. The exact behaviour contract of MotionEnabledStore
// (hydrate on construction, setEnabled persists AND republishes, forget
// cascades to both, isEnabled collapses absence to the default while the raw
// map keeps "absent" distinct from "explicitly set").
//
// One class for both directions because they differ only in their default; the
// two thin subclasses below pin those defaults where callers can name them.
// Mirrors dish-android's source/store/{Mic,Speaker}EnabledStore pair.

#pragma once

#include "architecture/StateSource.h"
#include "repository/AudioPreferenceRepository.h"

#include <map>
#include <string>

namespace dish::source {

// slotId -> enabled. std::map gives a deterministic, ==-comparable value so the
// Observable's distinct-until-changed suppresses no-op re-emits.
using AudioEnabledMap = std::map<std::string, bool>;

class AudioEnabledStore : public arch::StateSource<AudioEnabledMap> {
  public:
    // `repo` is borrowed, not owned — it outlives the store (both live on the
    // AppModel). `defaultEnabled` is the answer for a slot the user has never
    // touched.
    AudioEnabledStore(repository::AudioPreferenceRepository* repo, bool defaultEnabled);

    bool isEnabled(const std::string& slotId) const;
    void setEnabled(const std::string& slotId, bool enabled);
    // Drop the slot from both the repo and the live state (cascade forget). A
    // no-op (no emit) on the state side if it was absent.
    void forget(const std::string& slotId);

    bool defaultEnabled() const { return defaultEnabled_; }

  private:
    static AudioEnabledMap hydrate(repository::AudioPreferenceRepository* repo);

    repository::AudioPreferenceRepository* repo_;
    bool defaultEnabled_;
};

// The microphone toggle. DEFAULT OFF: capturing a microphone is opt-in per
// binding, never something a fresh install starts doing on its own — the
// privacy stance is the point, not a convenience default.
class MicEnabledStore final : public AudioEnabledStore {
  public:
    static constexpr bool kDefaultEnabled = false;
    explicit MicEnabledStore(repository::AudioPreferenceRepository* repo)
        : AudioEnabledStore(repo, kDefaultEnabled) {}
};

// The controller-speaker toggle. DEFAULT ON: playback carries nothing of the
// user's, so the useful default is the working one.
class SpeakerEnabledStore final : public AudioEnabledStore {
  public:
    static constexpr bool kDefaultEnabled = true;
    explicit SpeakerEnabledStore(repository::AudioPreferenceRepository* repo)
        : AudioEnabledStore(repo, kDefaultEnabled) {}
};

} // namespace dish::source
