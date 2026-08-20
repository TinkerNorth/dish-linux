// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The once-per-Live-stretch warm rule (android CatalogPrewarmer): a satellite
// is warmed on its Live edge, never re-warmed while the link stays Live, and
// re-armed by a drop so the next reconnect warms again.

#include "core/reducer/CatalogPrewarm.h"

#include <catch2/catch_test_macros.hpp>

#include <set>

using dish::reducer::catalogPrewarmTargets;

TEST_CASE("prewarm: a link entering Live is warmed exactly once", "[catalog][prewarm]") {
    std::set<QString> warmed;
    const auto first = catalogPrewarmTargets({{QStringLiteral("sat-a"), true}}, warmed);
    REQUIRE(first.size() == 1);
    CHECK(first[0] == QStringLiteral("sat-a"));

    // The steady state is free: the same live link never re-warms.
    CHECK(catalogPrewarmTargets({{QStringLiteral("sat-a"), true}}, warmed).empty());
    CHECK(catalogPrewarmTargets({{QStringLiteral("sat-a"), true}}, warmed).empty());
}

TEST_CASE("prewarm: a non-live link is never warmed", "[catalog][prewarm]") {
    std::set<QString> warmed;
    CHECK(catalogPrewarmTargets({{QStringLiteral("sat-a"), false}}, warmed).empty());
    CHECK(warmed.empty());
}

TEST_CASE("prewarm: a drop re-arms so the next Live warms again", "[catalog][prewarm]") {
    std::set<QString> warmed;
    REQUIRE(catalogPrewarmTargets({{QStringLiteral("sat-a"), true}}, warmed).size() == 1);

    // Link drops (still known, no longer Live) -> re-armed, nothing to warm.
    CHECK(catalogPrewarmTargets({{QStringLiteral("sat-a"), false}}, warmed).empty());
    // Reconnect -> a fresh warm.
    CHECK(catalogPrewarmTargets({{QStringLiteral("sat-a"), true}}, warmed).size() == 1);
}

TEST_CASE("prewarm: a link that vanishes entirely also re-arms", "[catalog][prewarm]") {
    std::set<QString> warmed;
    REQUIRE(catalogPrewarmTargets({{QStringLiteral("sat-a"), true}}, warmed).size() == 1);
    CHECK(catalogPrewarmTargets({}, warmed).empty());
    CHECK(catalogPrewarmTargets({{QStringLiteral("sat-a"), true}}, warmed).size() == 1);
}

TEST_CASE("prewarm: links are independent", "[catalog][prewarm]") {
    std::set<QString> warmed;
    const auto both = catalogPrewarmTargets(
        {{QStringLiteral("sat-a"), true}, {QStringLiteral("sat-b"), true}}, warmed);
    CHECK(both.size() == 2);

    // One drops; the other must not re-warm on the next pass.
    CHECK(catalogPrewarmTargets({{QStringLiteral("sat-a"), false}, {QStringLiteral("sat-b"), true}},
                                warmed)
              .empty());
    // Only the dropped one warms on its reconnect.
    const auto again = catalogPrewarmTargets(
        {{QStringLiteral("sat-a"), true}, {QStringLiteral("sat-b"), true}}, warmed);
    REQUIRE(again.size() == 1);
    CHECK(again[0] == QStringLiteral("sat-a"));
}
