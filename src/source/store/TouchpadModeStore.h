// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// TouchpadModeStore — a StateSource over the per-satellite touchpad-mode pick:
// satelliteId -> wire mode string ("off" | "ds4" | "mouse"). It bridges the
// durable TouchpadModeRepository (the source of truth across launches) with a
// reactive in-memory Observable<map>. Header-only.

#pragma once

#include "architecture/StateSource.h"
#include "core/reducer/TouchpadModeResolve.h"
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

    // The pick for a satellite, or nullopt when the user never picked one: no
    // invented default here, because each caller decides what absence means.
    std::optional<std::string> modeFor(const std::string& satelliteId) const {
        const auto& snapshot = state().value();
        const auto it = snapshot.find(satelliteId);
        if (it == snapshot.end()) { return std::nullopt; }
        return it->second;
    }

    // Persist + republish the pick for a satellite. A mode the repository would
    // refuse is refused here too, so memory never holds what disk will not.
    void setMode(const std::string& satelliteId, const std::string& mode) {
        if (!reducer::isValidTouchpadModeName(mode)) { return; }
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
