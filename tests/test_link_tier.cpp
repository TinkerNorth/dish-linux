// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The link-tier ladder is one ladder across the Dish clients: a Satellite link
// is the top rung, a Moonlight host the next, and the ranking key orders a
// mixed list best-first.

#include "core/reducer/ConnectionRows.h"
#include "core/reducer/LinkTier.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <vector>

using dish::reducer::ConnectionKind;
using dish::reducer::LinkTier;
using dish::reducer::linkTierFor;
using dish::reducer::linkTierRank;

TEST_CASE("tier: a Satellite link is Fastest", "[tier]") {
    REQUIRE(linkTierFor(ConnectionKind::Satellite) == LinkTier::Fastest);
}

TEST_CASE("tier: a Moonlight host is Fast", "[tier]") {
    REQUIRE(linkTierFor(ConnectionKind::Moonlight) == LinkTier::Fast);
}

TEST_CASE("tier: the rank orders best-first and Basic stays the last rung", "[tier]") {
    // Basic is dish-android's Bluetooth gamepad link; nothing here produces it,
    // but the ladder keeps it so the vocabulary is one ladder.
    REQUIRE(linkTierRank(LinkTier::Fastest) < linkTierRank(LinkTier::Fast));
    REQUIRE(linkTierRank(LinkTier::Fast) < linkTierRank(LinkTier::Basic));

    std::vector<ConnectionKind> kinds = {ConnectionKind::Moonlight, ConnectionKind::Satellite,
                                         ConnectionKind::Moonlight};
    std::stable_sort(kinds.begin(), kinds.end(), [](ConnectionKind a, ConnectionKind b) {
        return linkTierRank(linkTierFor(a)) < linkTierRank(linkTierFor(b));
    });
    REQUIRE(kinds.front() == ConnectionKind::Satellite);
}
