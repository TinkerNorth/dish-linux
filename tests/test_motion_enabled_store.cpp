// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "repository/MotionPreferenceRepository.h"
#include "source/store/MotionEnabledStore.h"

#include "QSettingsFixture.h"

#include <catch2/catch_test_macros.hpp>

#include <memory>

using dish::repository::MotionPreference;
using dish::repository::MotionPreferenceRepository;
using dish::source::MotionEnabledStore;
using dish::test::makeSharedSettings;

namespace {

std::unique_ptr<MotionPreferenceRepository>
seededRepo(std::initializer_list<MotionPreference> prefs) {
    auto repo = std::make_unique<MotionPreferenceRepository>(makeSharedSettings());
    for (const auto& p : prefs) { repo->put(p); }
    return repo;
}

} // namespace

TEST_CASE("hydrates state from the repository on construction", "[motion-store]") {
    auto repo = seededRepo({MotionPreference{"virtual", true}, MotionPreference{"9", false}});
    MotionEnabledStore store(repo.get());

    const auto& snapshot = store.state().value();
    REQUIRE(snapshot.size() == 2);
    CHECK(snapshot.at("virtual") == true);
    CHECK(snapshot.at("9") == false);
}

TEST_CASE("default is on for an unwritten slot", "[motion-store]") {
    auto repo = seededRepo({});
    MotionEnabledStore store(repo.get());
    CHECK(store.isEnabled("never-toggled"));
    CHECK(MotionEnabledStore::kDefaultEnabled);
}

TEST_CASE("setEnabled persists to the repository AND republishes state", "[motion-store]") {
    auto repo = seededRepo({});
    MotionEnabledStore store(repo.get());

    store.setEnabled("9", false);

    REQUIRE(repo->get("9").has_value());
    CHECK(repo->get("9")->enabled == false);
    CHECK(store.state().value().at("9") == false);
    CHECK_FALSE(store.isEnabled("9"));
}

TEST_CASE("setEnabled with opposite values flips both layers", "[motion-store]") {
    auto repo = seededRepo({});
    MotionEnabledStore store(repo.get());
    store.setEnabled("9", false);
    store.setEnabled("9", true);
    store.setEnabled("9", false);

    CHECK(repo->get("9")->enabled == false);
    CHECK(store.state().value().at("9") == false);
    CHECK_FALSE(store.isEnabled("9"));
}

TEST_CASE("forget removes the entry from state AND from the repository", "[motion-store]") {
    auto repo = seededRepo({MotionPreference{"9", false}});
    MotionEnabledStore store(repo.get());
    CHECK_FALSE(store.isEnabled("9"));

    store.forget("9");

    CHECK_FALSE(repo->get("9").has_value());
    // Absent from state means the default (on) applies again.
    CHECK(store.isEnabled("9"));
    // state().value() returns a fresh copy per call, so find() and end() must
    // come from the same snapshot to be comparable iterators.
    const auto snapshot = store.state().value();
    CHECK(snapshot.find("9") == snapshot.end());
}

TEST_CASE("setEnabled for slot A leaves slot B unchanged", "[motion-store]") {
    auto repo = seededRepo({});
    MotionEnabledStore store(repo.get());

    store.setEnabled("virtual", true);
    store.setEnabled("9", false);
    store.setEnabled("virtual", false);

    CHECK(store.state().value().at("virtual") == false);
    CHECK(store.state().value().at("9") == false);
}

TEST_CASE("isEnabled for an explicitly disabled slot returns false", "[motion-store]") {
    auto repo = seededRepo({MotionPreference{"9", false}});
    MotionEnabledStore store(repo.get());
    CHECK_FALSE(store.isEnabled("9"));
}
