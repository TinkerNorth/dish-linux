// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "source/store/TouchpadModeStore.h"

#include "QSettingsFixture.h"
#include "StateSourceProbe.h"

#include <catch2/catch_test_macros.hpp>

using dish::repository::TouchpadModePreference;
using dish::repository::TouchpadModeRepository;
using dish::source::TouchpadModeMap;
using dish::source::TouchpadModeStore;
using dish::test::makeSharedSettings;

TEST_CASE("store hydrates from the repository on construction", "[touchpad-mode-store]") {
    auto settings = makeSharedSettings();
    TouchpadModeRepository repo(settings);
    repo.put(TouchpadModePreference{"sat-a", "ds4"});
    repo.put(TouchpadModePreference{"sat-b", "off"});

    TouchpadModeStore store(&repo);
    REQUIRE(store.modeFor("sat-a") == "ds4");
    REQUIRE(store.modeFor("sat-b") == "off");
}

TEST_CASE("never-picked reads nullopt, distinct from off", "[touchpad-mode-store]") {
    TouchpadModeRepository repo(makeSharedSettings());
    TouchpadModeStore store(&repo);
    CHECK_FALSE(store.modeFor("never").has_value());
    store.setMode("never", "off");
    REQUIRE(store.modeFor("never") == "off");
}

TEST_CASE("setMode persists to the repo AND republishes", "[touchpad-mode-store]") {
    auto settings = makeSharedSettings();
    TouchpadModeRepository repo(settings);
    TouchpadModeStore store(&repo);
    dish::test::StateSourceProbe<TouchpadModeMap> probe(store.state());
    const auto baseline = probe.count(); // the subscribe replay of the current state

    store.setMode("sat", "ds4");
    REQUIRE(store.modeFor("sat") == "ds4");
    TouchpadModeRepository repo2(settings);
    REQUIRE(repo2.get("sat").has_value());
    CHECK(repo2.get("sat")->mode == "ds4");
    REQUIRE(probe.count() == baseline + 1);
    CHECK(probe.latest().at("sat") == "ds4");
}

TEST_CASE("a same-value setMode does not re-emit", "[touchpad-mode-store]") {
    TouchpadModeRepository repo(makeSharedSettings());
    TouchpadModeStore store(&repo);
    store.setMode("sat", "ds4");
    dish::test::StateSourceProbe<TouchpadModeMap> probe(store.state());
    const auto baseline = probe.count();
    store.setMode("sat", "ds4");
    CHECK(probe.count() == baseline);
}

TEST_CASE("forget removes from the repo and the state", "[touchpad-mode-store]") {
    auto settings = makeSharedSettings();
    TouchpadModeRepository repo(settings);
    TouchpadModeStore store(&repo);
    store.setMode("sat", "mouse");

    store.forget("sat");
    CHECK_FALSE(store.modeFor("sat").has_value());
    CHECK_FALSE(repo.get("sat").has_value());
}

TEST_CASE("forget of an absent satellite emits nothing", "[touchpad-mode-store]") {
    TouchpadModeRepository repo(makeSharedSettings());
    TouchpadModeStore store(&repo);
    dish::test::StateSourceProbe<TouchpadModeMap> probe(store.state());
    const auto baseline = probe.count();
    store.forget("ghost");
    CHECK(probe.count() == baseline);
}
