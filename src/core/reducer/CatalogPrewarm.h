// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The once-per-Live-stretch edge detector behind catalog prewarming (the
// android CatalogPrewarmer rule): fetch a satellite's catalog when its link
// enters Live, and again only after the link has dropped and come back. It
// observes the raw link states, not the composed picker flow, so warming never
// perturbs any UI state.

#pragma once

#include <QString>

#include <set>
#include <utility>
#include <vector>

namespace dish::reducer {

// One row per known satellite link: (satellite id, link is Live right now).
using CatalogLink = std::pair<QString, bool>;

// Returns the ids to warm NOW, updating `warmed` in place: an id is armed on
// its Idle->Live edge and re-armed the first pass its link is no longer Live
// (or the link is gone entirely), so a reconnect warms again while a stable
// Live session never re-fetches.
inline std::vector<QString> catalogPrewarmTargets(const std::vector<CatalogLink>& links,
                                                  std::set<QString>& warmed) {
    std::set<QString> liveNow;
    for (const auto& [id, live] : links) {
        if (live) { liveNow.insert(id); }
    }
    for (auto it = warmed.begin(); it != warmed.end();) {
        if (liveNow.count(*it) == 0) {
            it = warmed.erase(it);
        } else {
            ++it;
        }
    }
    std::vector<QString> out;
    for (const auto& id : liveNow) {
        if (warmed.insert(id).second) { out.push_back(id); }
    }
    return out;
}

} // namespace dish::reducer
