// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// A slot's picker offers the live unbound connections plus its own bound one,
// which is held over even while offline.

#include "Models/Models.h"
#include "core/reducer/PickerVisibility.h"

#include <catch2/catch_test_macros.hpp>

#include <QList>
#include <QString>

#include <optional>

using dish::models::CatalogDto;
using dish::models::CatalogFeatureDto;
using dish::models::CatalogTypeDto;
using dish::models::ConnectionSummary;
using dish::models::LinkState;
using dish::reducer::connectionsVisibleInPicker;
using dish::reducer::isAvailableForPicker;
using dish::reducer::isFeatureOffered;
using dish::reducer::isTypeOfferable;
using dish::reducer::seedControllerType;

namespace {

ConnectionSummary summary(const QString& id, LinkState live) {
    ConnectionSummary c;
    c.id = id;
    c.label = id;
    c.live = live;
    return c;
}

QList<QString> ids(const QList<ConnectionSummary>& list) {
    QList<QString> out;
    for (const auto& c : list) { out.push_back(c.id); }
    return out;
}

CatalogTypeDto typeRow(int id, const QString& name) {
    CatalogTypeDto t;
    t.id = id;
    t.name = name;
    return t;
}

const std::optional<QString> kUnbound = std::nullopt;

} // namespace

TEST_CASE("picker: Connected counts as available", "[picker]") {
    REQUIRE(isAvailableForPicker(LinkState::Connected));
}

TEST_CASE("picker: Unstable counts as available (faltering but usable)", "[picker]") {
    REQUIRE(isAvailableForPicker(LinkState::Unstable));
}

TEST_CASE("picker: Connecting is not available (no live session yet)", "[picker]") {
    REQUIRE_FALSE(isAvailableForPicker(LinkState::Connecting));
}

TEST_CASE("picker: Ready is not available (paired and seen, not connected)", "[picker]") {
    REQUIRE_FALSE(isAvailableForPicker(LinkState::Ready));
}

TEST_CASE("picker: Found is not available (visible target, not connected)", "[picker]") {
    REQUIRE_FALSE(isAvailableForPicker(LinkState::Found));
}

TEST_CASE("picker: Saved is not available (offline)", "[picker]") {
    REQUIRE_FALSE(isAvailableForPicker(LinkState::Saved));
}

TEST_CASE("picker: Stale is not available (needs re-pairing)", "[picker]") {
    REQUIRE_FALSE(isAvailableForPicker(LinkState::Stale));
}

TEST_CASE("picker: every LinkState resolves through the predicate without throwing", "[picker]") {
    for (auto s : {LinkState::Found, LinkState::Stale, LinkState::Saved, LinkState::Ready,
                   LinkState::Connecting, LinkState::Connected, LinkState::Unstable}) {
        (void)isAvailableForPicker(s);
    }
    SUCCEED();
}

TEST_CASE("picker: empty input returns empty regardless of bind state", "[picker]") {
    REQUIRE(connectionsVisibleInPicker({}, kUnbound).isEmpty());
    REQUIRE(connectionsVisibleInPicker({}, std::optional<QString>("s:anything")).isEmpty());
}

TEST_CASE("picker: bound id not in the list is a no-op", "[picker]") {
    const auto online = summary("s:1", LinkState::Connected);
    const auto visible = connectionsVisibleInPicker({online}, std::optional<QString>("s:gone"));
    REQUIRE(ids(visible) == QList<QString>{"s:1"});
}

TEST_CASE("picker: an unavailable unbound connection is hidden", "[picker]") {
    const auto offline = summary("s:1", LinkState::Saved);
    REQUIRE(connectionsVisibleInPicker({offline}, kUnbound).isEmpty());
}

TEST_CASE("picker: an unavailable bound connection stays visible as the holdover", "[picker]") {
    const auto bound = summary("s:1", LinkState::Saved);
    REQUIRE(ids(connectionsVisibleInPicker({bound}, std::optional<QString>("s:1"))) ==
            QList<QString>{"s:1"});
}

TEST_CASE("picker: unbinding an offline connection makes it disappear", "[picker]") {
    const auto offline = summary("s:1", LinkState::Saved);
    REQUIRE(ids(connectionsVisibleInPicker({offline}, std::optional<QString>("s:1"))) ==
            QList<QString>{"s:1"});
    REQUIRE(connectionsVisibleInPicker({offline}, kUnbound).isEmpty());
}

TEST_CASE("picker: auto-recovery brings the bound row back to a normal available render",
          "[picker]") {
    const auto offlineBound = summary("s:1", LinkState::Saved);
    const auto recoveredBound = summary("s:1", LinkState::Connected);
    REQUIRE(ids(connectionsVisibleInPicker({offlineBound}, std::optional<QString>("s:1"))) ==
            QList<QString>{"s:1"});
    REQUIRE(ids(connectionsVisibleInPicker({recoveredBound}, std::optional<QString>("s:1"))) ==
            QList<QString>{"s:1"});
}

TEST_CASE("picker: Stale bound holdover is preserved until re-pair or unbind", "[picker]") {
    const auto stale = summary("s:1", LinkState::Stale);
    REQUIRE(ids(connectionsVisibleInPicker({stale}, std::optional<QString>("s:1"))) ==
            QList<QString>{"s:1"});
    REQUIRE(connectionsVisibleInPicker({stale}, kUnbound).isEmpty());
}

TEST_CASE("picker: keeps only the live unbound entries", "[picker]") {
    const auto online = summary("s:online", LinkState::Connected);
    const auto ready = summary("s:ready", LinkState::Ready);
    const auto offline = summary("s:offline", LinkState::Saved);
    const auto needsPair = summary("s:stale", LinkState::Stale);
    const auto visible = connectionsVisibleInPicker({online, ready, offline, needsPair}, kUnbound);
    REQUIRE(ids(visible) == QList<QString>{"s:online"});
}

TEST_CASE("picker: shows the bound offline entry alongside other available ones", "[picker]") {
    const auto onlineAlt = summary("s:online", LinkState::Connected);
    const auto boundOffline = summary("s:bound", LinkState::Saved);
    const auto unboundOffline = summary("s:other", LinkState::Saved);
    const auto visible = connectionsVisibleInPicker({onlineAlt, boundOffline, unboundOffline},
                                                    std::optional<QString>("s:bound"));
    REQUIRE(ids(visible) == QList<QString>({"s:online", "s:bound"}));
}

TEST_CASE("picker: preserves the input order", "[picker]") {
    const auto a = summary("s:1", LinkState::Connected);
    const auto b = summary("s:2", LinkState::Connected);
    const auto c = summary("s:3", LinkState::Connected);
    const auto visible = connectionsVisibleInPicker({a, b, c}, std::optional<QString>("s:3"));
    REQUIRE(ids(visible) == QList<QString>({"s:1", "s:2", "s:3"}));
}

TEST_CASE("picker: preserves order even with mixed available and held-over rows", "[picker]") {
    const auto a = summary("s:a", LinkState::Connected);
    const auto bSaved = summary("s:b", LinkState::Saved);
    const auto cConnecting = summary("s:c", LinkState::Connecting);
    const auto dSavedUnbound = summary("s:d", LinkState::Saved);
    const auto e = summary("s:e", LinkState::Connected);
    const auto visible = connectionsVisibleInPicker({a, bSaved, cConnecting, dSavedUnbound, e},
                                                    std::optional<QString>("s:b"));
    REQUIRE(ids(visible) == QList<QString>({"s:a", "s:b", "s:e"}));
}

TEST_CASE("picker: keeps exactly one held-over row even when multiple are unreachable",
          "[picker]") {
    QList<ConnectionSummary> offlineConns;
    for (int i = 1; i <= 6; ++i) {
        offlineConns.push_back(summary(QStringLiteral("s:%1").arg(i), LinkState::Saved));
    }
    const auto visible = connectionsVisibleInPicker(offlineConns, std::optional<QString>("s:4"));
    REQUIRE(ids(visible) == QList<QString>{"s:4"});
}

TEST_CASE("picker: filter does not mutate the input list", "[picker]") {
    QList<ConnectionSummary> input{summary("s:1", LinkState::Connected),
                                   summary("s:2", LinkState::Saved),
                                   summary("s:3", LinkState::Stale)};
    const auto snapshot = input;
    connectionsVisibleInPicker(input, std::optional<QString>("s:2"));
    REQUIRE(ids(input) == ids(snapshot));
    REQUIRE(input.size() == 3);
}

TEST_CASE("picker: idempotent - running it twice on its own output is stable", "[picker]") {
    const QList<ConnectionSummary> input{
        summary("s:1", LinkState::Connected), summary("s:2", LinkState::Saved),
        summary("s:3", LinkState::Stale), summary("s:4", LinkState::Ready)};
    const auto once = connectionsVisibleInPicker(input, std::optional<QString>("s:2"));
    const auto twice = connectionsVisibleInPicker(once, std::optional<QString>("s:2"));
    REQUIRE(ids(once) == ids(twice));
}

TEST_CASE("picker: bound id of empty string matches nothing", "[picker]") {
    const auto saved = summary("s:1", LinkState::Saved);
    REQUIRE(connectionsVisibleInPicker({saved}, std::optional<QString>("")).isEmpty());
}

TEST_CASE("picker: cross product of state and bind status matches the spec table", "[picker]") {
    for (auto state : {LinkState::Found, LinkState::Stale, LinkState::Saved, LinkState::Ready,
                       LinkState::Connecting, LinkState::Connected, LinkState::Unstable}) {
        const auto c = summary("s:probe", state);

        REQUIRE(ids(connectionsVisibleInPicker({c}, std::optional<QString>("s:probe"))) ==
                QList<QString>{"s:probe"});

        const auto whenUnbound = connectionsVisibleInPicker({c}, kUnbound);
        if (isAvailableForPicker(state)) {
            REQUIRE(ids(whenUnbound) == QList<QString>{"s:probe"});
        } else {
            REQUIRE(whenUnbound.isEmpty());
        }
    }
}

TEST_CASE("picker: only live connections populate the picker for an unbound slot", "[picker]") {
    const auto live = summary("s:1", LinkState::Connected);
    const auto ready = summary("s:2", LinkState::Ready);
    const auto found = summary("s:3", LinkState::Found);
    REQUIRE(ids(connectionsVisibleInPicker({live, ready, found}, kUnbound)) ==
            QList<QString>{"s:1"});
}

TEST_CASE("picker: remembered-but-offline satellites are hidden from an unbound slot", "[picker]") {
    const auto offline = summary("s:1", LinkState::Saved);
    REQUIRE(connectionsVisibleInPicker({offline}, kUnbound).isEmpty());
}

TEST_CASE("picker: two slots bound to two live connections each see both", "[picker]") {
    const auto a = summary("s:a", LinkState::Connected);
    const auto b = summary("s:b", LinkState::Connected);
    REQUIRE(ids(connectionsVisibleInPicker({a, b}, std::optional<QString>("s:a"))) ==
            QList<QString>({"s:a", "s:b"}));
    REQUIRE(ids(connectionsVisibleInPicker({a, b}, std::optional<QString>("s:b"))) ==
            QList<QString>({"s:a", "s:b"}));
    REQUIRE(ids(connectionsVisibleInPicker({a, b}, kUnbound)) == QList<QString>({"s:a", "s:b"}));
}

TEST_CASE("picker: slot A keeps offline holdover, slot B does not (per-slot bind)", "[picker]") {
    const auto sat1Off = summary("s:1", LinkState::Saved);
    const auto sat2On = summary("s:2", LinkState::Connected);
    REQUIRE(ids(connectionsVisibleInPicker({sat1Off, sat2On}, std::optional<QString>("s:1"))) ==
            QList<QString>({"s:1", "s:2"}));
    REQUIRE(ids(connectionsVisibleInPicker({sat1Off, sat2On}, kUnbound)) == QList<QString>{"s:2"});
}

TEST_CASE("picker: crowded - one online, one connecting, one stale-held, one offline-unbound",
          "[picker]") {
    const auto online = summary("s:1", LinkState::Connected);
    const auto connecting = summary("s:2", LinkState::Connecting);
    const auto staleHeld = summary("s:3", LinkState::Stale);
    const auto offlineUnbound = summary("s:4", LinkState::Saved);
    const auto picker = connectionsVisibleInPicker({online, connecting, staleHeld, offlineUnbound},
                                                   std::optional<QString>("s:3"));
    REQUIRE(ids(picker) == QList<QString>({"s:1", "s:3"}));
}

TEST_CASE("picker: a known controller type is offerable", "[picker][catalog]") {
    CatalogTypeDto t;
    t.id = 0;
    t.slug = "xbox360";
    t.name = "Xbox 360 Controller";
    REQUIRE(isTypeOfferable(t));
}

TEST_CASE("picker: an unknown-slug type newer than the app still renders (offerable)",
          "[picker][catalog]") {
    CatalogTypeDto t;
    t.id = 7;
    t.slug = "hyperpad";
    t.name = "HyperPad 9000";
    REQUIRE(isTypeOfferable(t));
}

TEST_CASE("picker: a nameless catalog row is not offerable", "[picker][catalog]") {
    CatalogTypeDto t;
    t.id = 9;
    t.slug = "ghost";
    REQUIRE_FALSE(isTypeOfferable(t));
}

TEST_CASE("picker: a known supported feature is offered", "[picker][catalog]") {
    CatalogTypeDto t;
    CatalogFeatureDto f;
    f.supported = true;
    t.features.insert("rumble", f);
    const QList<QString> known{"rumble", "motion", "lightbar", "analogTriggers", "touchpad"};
    REQUIRE(isFeatureOffered(t, "rumble", known));
}

TEST_CASE("picker: an unknown feature slug is silently not offered", "[picker][catalog]") {
    CatalogTypeDto t;
    CatalogFeatureDto warp;
    warp.supported = true; // server says supported, but the client has no code
    t.features.insert("warp", warp);
    const QList<QString> known{"rumble", "motion", "lightbar", "analogTriggers", "touchpad"};
    REQUIRE_FALSE(isFeatureOffered(t, "warp", known));
}

TEST_CASE("picker: a known but unsupported feature is not offered", "[picker][catalog]") {
    CatalogTypeDto t;
    CatalogFeatureDto motion;
    motion.supported = false;
    t.features.insert("motion", motion);
    const QList<QString> known{"rumble", "motion", "lightbar", "analogTriggers", "touchpad"};
    REQUIRE_FALSE(isFeatureOffered(t, "motion", known));
}

TEST_CASE("seed: defaults to the catalog's first offered type (id 0)", "[picker][catalog]") {
    CatalogDto c;
    c.controllerTypes.push_back(typeRow(0, "Xbox 360 Controller"));
    c.controllerTypes.push_back(typeRow(1, "DualShock 4"));
    REQUIRE(seedControllerType(std::nullopt, c) == 0);
}

TEST_CASE("seed: defaults to the catalog's first offered type when it is id 1",
          "[picker][catalog]") {
    CatalogDto c;
    c.controllerTypes.push_back(typeRow(1, "DualShock 4"));
    c.controllerTypes.push_back(typeRow(0, "Xbox 360 Controller"));
    REQUIRE(seedControllerType(std::nullopt, c) == 1);
}

TEST_CASE("seed: the user override wins over the catalog default", "[picker][catalog]") {
    CatalogDto c;
    c.controllerTypes.push_back(typeRow(1, "DualShock 4"));
    REQUIRE(seedControllerType(3, c) == 3);
}

TEST_CASE("seed: no cached catalog falls back to Xbox (0)", "[picker][catalog]") {
    REQUIRE(seedControllerType(std::nullopt, std::nullopt) == 0);
}

TEST_CASE("seed: the override still wins with no catalog cached", "[picker][catalog]") {
    REQUIRE(seedControllerType(2, std::nullopt) == 2);
}

TEST_CASE("seed: skips a nameless first row - the default is the first OFFERABLE type",
          "[picker][catalog]") {
    CatalogDto c;
    CatalogTypeDto ghost;
    ghost.id = 9;
    ghost.slug = "ghost";
    c.controllerTypes.push_back(ghost);
    c.controllerTypes.push_back(typeRow(1, "DualShock 4"));
    REQUIRE(seedControllerType(std::nullopt, c) == 1);
}

TEST_CASE("seed: a cached-but-empty catalog falls back to Xbox (0)", "[picker][catalog]") {
    REQUIRE(seedControllerType(std::nullopt, CatalogDto{}) == 0);
}
