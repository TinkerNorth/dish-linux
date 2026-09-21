// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The link-tier ladder: how fast and how stable a kind of host link is, ranked
// best-first. One ladder across the Dish clients, so a user who reads "Fastest"
// beside a satellite on the phone reads the same word beside it here:
//
//   Fastest  a Satellite link. Our own wire, one hop, every feature.
//   Fast     a Moonlight host's control stream (Sunshine, Apollo, Wolf).
//   Basic    a Bluetooth gamepad link. dish-android offers one (the phone as a
//            Bluetooth HID pad); this client does not, so nothing here produces
//            it. It stays in the enum so the vocabulary is one ladder and not
//            two that happen to agree.
//
// Returns tokens only; localized copy lives in QML (shared/LinkVocabulary.qml).

#pragma once

#include "core/reducer/ConnectionRows.h"

#include <cstdint>

namespace dish::reducer {

// Declaration order is the ranking: lower is better.
enum class LinkTier : std::uint8_t { Fastest, Fast, Basic };

inline LinkTier linkTierFor(ConnectionKind kind) {
    switch (kind) {
    case ConnectionKind::Moonlight:
        return LinkTier::Fast;
    case ConnectionKind::Satellite:
    default:
        return LinkTier::Fastest;
    }
}

// Best-first ordering key for a list that mixes kinds: a smaller rank sorts
// earlier, the way dish-android's byTier comparator orders its hosts.
inline int linkTierRank(LinkTier tier) { return static_cast<int>(tier); }

} // namespace dish::reducer
