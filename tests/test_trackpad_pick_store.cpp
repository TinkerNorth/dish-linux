// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "source/store/TrackpadPickStore.h"

#include "QSettingsFixture.h"
#include "StateSourceProbe.h"

#include <catch2/catch_test_macros.hpp>

using dish::repository::TrackpadPickPreference;
using dish::repository::TrackpadPickRepository;
using dish::source::TrackpadPickMap;
using dish::source::TrackpadPickStore;
using dish::test::makeSharedSettings;

TEST_CASE("the pick store hydrates from the repository on construction", "[trackpad-pick-store]") {
    auto settings = makeSharedSettings();
    TrackpadPickRepository repo(settings);
    repo.put(TrackpadPickPreference{"slot-a", "pad"});
    repo.put(TrackpadPickPreference{"slot-b", "off"});
    TrackpadPickStore store(&repo);
    REQUIRE(store.pickFor("slot-a") == "pad");
    REQUIRE(store.pickFor("slot-b") == "off");
}

TEST_CASE("a never-picked slot reads nullopt, distinct from off", "[trackpad-pick-store]") {
    TrackpadPickRepository repo(makeSharedSettings());
    TrackpadPickStore store(&repo);
    CHECK_FALSE(store.pickFor("never").has_value());
    store.setPick("never", "off");
    REQUIRE(store.pickFor("never") == "off");
}

TEST_CASE("setPick persists to the repo AND republishes", "[trackpad-pick-store]") {
    auto settings = makeSharedSettings();
    TrackpadPickRepository repo(settings);
    TrackpadPickStore store(&repo);
    dish::test::StateSourceProbe<TrackpadPickMap> probe(store.state());
    const auto baseline = probe.count();
    store.setPick("slot", "stick");
    REQUIRE(store.pickFor("slot") == "stick");
    TrackpadPickRepository repo2(settings);
    REQUIRE(repo2.get("slot").has_value());
    CHECK(repo2.get("slot")->pick == "stick");
    REQUIRE(probe.count() == baseline + 1);
    CHECK(probe.latest().at("slot") == "stick");
}

TEST_CASE("a same-value setPick does not re-emit", "[trackpad-pick-store]") {
    TrackpadPickRepository repo(makeSharedSettings());
    TrackpadPickStore store(&repo);
    store.setPick("slot", "pad");
    dish::test::StateSourceProbe<TrackpadPickMap> probe(store.state());
    const auto baseline = probe.count();
    store.setPick("slot", "pad");
    CHECK(probe.count() == baseline);
}

TEST_CASE("a pick the repository refuses is not held in memory either", "[trackpad-pick-store]") {
    TrackpadPickRepository repo(makeSharedSettings());
    TrackpadPickStore store(&repo);
    store.setPick("slot", "pad");
    dish::test::StateSourceProbe<TrackpadPickMap> probe(store.state());
    const auto baseline = probe.count();
    store.setPick("slot", "ds4");
    CHECK(store.pickFor("slot") == "pad");
    CHECK(probe.count() == baseline);
    store.setPick("fresh", "banana");
    CHECK_FALSE(store.pickFor("fresh").has_value());
}
