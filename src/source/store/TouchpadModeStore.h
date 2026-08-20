// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// TouchpadModeStore — a StateSource over the per-satellite touchpad-mode pick:
// satelliteId -> wire mode string ("off" | "ds4" | "mouse"). It bridges the
// durable TouchpadModeRepository (the source of truth across launches) with a
// reactive in-memory Observable<map> the TouchpadModeComposer reads. Mirrors
// dish-android source/store/TouchpadModeStore (an
// AbstractStateSource<Map<String,String>> over the same repo). Header-only.
//
// Behaviour the android tests pin and this preserves:
//   * Hydrates its state from repo.all() on construction.
//   * setMode(satelliteId, mode) persists to the repo AND republishes state.
//   * forget(satelliteId) removes from BOTH the repo and the state (cascade
//     forget, e.g. when a satellite is unpaired).
//   * modeFor(satelliteId) returns nullopt for a satellite the user has never
//     picked for — "never picked" is DISTINCT from "off", and the resolve
//     ladder (not this store) collapses absence to the pair-time default.
// A same-value setMode still writes through to the repo but the Observable's
// == compare suppresses the redundant re-emit; forget of an absent satellite
// short-circuits the state write entirely.

#pragma once

#include "architecture/StateSource.h"
#include "repository/TouchpadModeRepository.h"

#include <QString>

#include <map>
#include <optional>
#include <string>

namespace dish::source {

// satelliteId -> picked wire mode. std::map gives a deterministic,
// ==-comparable value so the Observable's distinct-until-changed suppresses
// no-op re-emits.
using TouchpadModeMap = std::map<std::string, std::string>;

class TouchpadModeStore : public arch::StateSource<TouchpadModeMap> {
  public:
    // `repo` is the durable backing store; it is read once at construction to
    // hydrate the initial state and written through on every mutation. Borrowed,
    // not owned — it outlives the store (owned by the AppModel).
    explicit TouchpadModeStore(repository::TouchpadModeRepository* repo)
        : arch::StateSource<TouchpadModeMap>(hydrate(repo)), repo_(repo) {}

    // The pick for a satellite, or nullopt when the user never picked one —
    // no invented default here (the resolve ladder owns the collapse to off).
    std::optional<std::string> modeFor(const std::string& satelliteId) const {
        const auto& snapshot = state().value();
        const auto it = snapshot.find(satelliteId);
        if (it == snapshot.end()) { return std::nullopt; }
        return it->second;
    }

    // Persist + republish the pick for a satellite.
    void setMode(const std::string& satelliteId, const std::string& mode) {
        if (repo_ != nullptr) {
            repo_->put(repository::TouchpadModePreference{QString::fromStdString(satelliteId),
                                                          QString::fromStdString(mode)});
        }
        setState([&](const TouchpadModeMap& current) {
            TouchpadModeMap next = current;
            next[satelliteId] = mode;
            return next;
        });
    }

    // Drop the satellite from both the repo and the live state (cascade
    // forget). A no-op (no emit) on the state side if it was absent.
    void forget(const std::string& satelliteId) {
        if (repo_ != nullptr) { repo_->remove(QString::fromStdString(satelliteId)); }
        setState([&](const TouchpadModeMap& current) {
            if (current.find(satelliteId) == current.end()) { return current; }
            TouchpadModeMap next = current;
            next.erase(satelliteId);
            return next;
        });
    }

  private:
    static TouchpadModeMap hydrate(repository::TouchpadModeRepository* repo) {
        TouchpadModeMap out;
        if (repo == nullptr) { return out; }
        for (const auto& pref : repo->all()) {
            out[pref.satelliteId.toStdString()] = pref.mode.toStdString();
        }
        return out;
    }

    repository::TouchpadModeRepository* repo_;
};

} // namespace dish::source
