// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// TrackpadPickStore — a StateSource over the per-slot trackpad pick of the Steam
// Controllers: slotId -> "stick" | "pad" | "mouse" | "off". It bridges the
// durable TrackpadPickRepository with a reactive in-memory Observable<map>, the
// shape TouchpadModeStore has for the per-satellite pick. Header-only.

#pragma once

#include "architecture/StateSource.h"
#include "core/reducer/TrackpadPickResolve.h"
#include "repository/TrackpadPickRepository.h"

#include <QString>

#include <map>
#include <optional>
#include <string>

namespace dish::source {

using TrackpadPickMap = std::map<std::string, std::string>;

class TrackpadPickStore : public arch::StateSource<TrackpadPickMap> {
  public:
    // `repo` is the durable backing store, read once to hydrate and written through
    // on every mutation. Borrowed, not owned: it outlives the store.
    explicit TrackpadPickStore(repository::TrackpadPickRepository* repo)
        : arch::StateSource<TrackpadPickMap>(hydrate(repo)), repo_(repo) {}

    // The pick for a slot, or nullopt when the user never picked one: the ladder
    // decides what absence means, per model.
    std::optional<std::string> pickFor(const std::string& slotId) const {
        const auto& snapshot = state().value();
        const auto it = snapshot.find(slotId);
        if (it == snapshot.end()) { return std::nullopt; }
        return it->second;
    }

    // Persist + republish. A pick the repository would refuse is refused here too.
    void setPick(const std::string& slotId, const std::string& pick) {
        if (!reducer::isValidTrackpadPickName(pick)) { return; }
        if (repo_ != nullptr) {
            repo_->put(repository::TrackpadPickPreference{QString::fromStdString(slotId),
                                                          QString::fromStdString(pick)});
        }
        setState([&](const TrackpadPickMap& current) {
            TrackpadPickMap next = current;
            next[slotId] = pick;
            return next;
        });
    }

  private:
    static TrackpadPickMap hydrate(repository::TrackpadPickRepository* repo) {
        TrackpadPickMap out;
        if (repo == nullptr) { return out; }
        for (const auto& pref : repo->all()) {
            out[pref.slotId.toStdString()] = pref.pick.toStdString();
        }
        return out;
    }

    repository::TrackpadPickRepository* repo_;
};

} // namespace dish::source
