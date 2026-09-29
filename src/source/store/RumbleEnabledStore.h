// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// RumbleEnabledStore: a StateSource over the per-slot rumble switch, slotId ->
// bool, hydrated from the durable RumblePreferenceRepository and written through
// on every change, the way MotionEnabledStore keeps motion. Mirrors dish-android
// source/store/RumbleEnabledStore: the switch decides what of the host's rumble
// reaches the pad (reducer::rumbleTheUserAllows), never the descriptor, so the
// host keeps a pad it can rumble and a flip needs no re-registration.

#pragma once

#include "architecture/StateSource.h"
#include "repository/RumblePreferenceRepository.h"

#include <map>
#include <string>

namespace dish::source {

// slotId -> enabled. std::map gives a deterministic, ==-comparable value so the
// Observable's distinct-until-changed suppresses no-op re-emits.
using RumbleEnabledMap = std::map<std::string, bool>;

class RumbleEnabledStore final : public arch::StateSource<RumbleEnabledMap> {
  public:
    // A slot the user has never switched rumbles.
    static constexpr bool kDefaultEnabled = true;

    // `repo` is read once to hydrate and written through on every change.
    // Borrowed: it outlives the store (both live on the AppModel).
    explicit RumbleEnabledStore(repository::RumblePreferenceRepository* repo);

    bool isEnabled(const std::string& slotId) const;
    void setEnabled(const std::string& slotId, bool enabled);

  private:
    static RumbleEnabledMap hydrate(repository::RumblePreferenceRepository* repo);

    repository::RumblePreferenceRepository* repo_;
};

} // namespace dish::source
