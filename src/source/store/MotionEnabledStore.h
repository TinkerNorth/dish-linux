// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// MotionEnabledStore — a StateSource over the per-slot motion-enable toggle:
// slotId -> bool. It bridges the durable MotionPreferenceRepository (the source
// of truth across launches) with a reactive in-memory Observable<map> the
// MotionCapabilityComposer reads. Mirrors dish-android source/store/
// MotionEnabledStore (an AbstractStateSource<Map<String,Boolean>> over the same
// repo).
//
// Behaviour the android tests pin and this preserves:
//   * Hydrates its state from repo.all() on construction.
//   * setEnabled(slot, on) persists to the repo AND republishes state.
//   * forget(slot) removes from BOTH the repo and the state (cascade forget,
//     e.g. when a slot's connection is forgotten).
//   * isEnabled(slot) collapses an absent entry to DEFAULT_ENABLED (true), while
//     the raw state map keeps "absent" distinct from "explicitly true" so the
//     composer can tell undecided from on.

#pragma once

#include "architecture/StateSource.h"
#include "repository/MotionPreferenceRepository.h"

#include <QString>

#include <map>
#include <string>

namespace dish::source {

// slotId -> enabled. std::map gives a deterministic, ==-comparable value so the
// Observable's distinct-until-changed suppresses no-op re-emits.
using MotionEnabledMap = std::map<std::string, bool>;

class MotionEnabledStore : public arch::StateSource<MotionEnabledMap> {
  public:
    // Default toggle for a slot the user has never touched: motion ON.
    static constexpr bool kDefaultEnabled = true;

    // `repo` is the durable backing store; it is read once at construction to
    // hydrate the initial state and written through on every mutation. Borrowed,
    // not owned — it outlives the store (owned by the AppModel).
    explicit MotionEnabledStore(repository::MotionPreferenceRepository* repo);

    // The toggle for a slot, defaulting an unwritten slot to kDefaultEnabled.
    bool isEnabled(const std::string& slotId) const;

    // Persist + republish the toggle for a slot.
    void setEnabled(const std::string& slotId, bool enabled);

    // Drop the slot from both the repo and the live state (cascade forget). A
    // no-op (no emit) on the state side if it was absent.
    void forget(const std::string& slotId);

  private:
    static MotionEnabledMap hydrate(repository::MotionPreferenceRepository* repo);

    repository::MotionPreferenceRepository* repo_;
};

} // namespace dish::source
