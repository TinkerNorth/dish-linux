// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The binding pages seed their Rumble switch from isEnabled() (App.rumbleEnabledFor
// forwards to it), so what these read back is what the user sees when a page
// reopens.

#include "repository/RumblePreferenceRepository.h"
#include "source/store/RumbleEnabledStore.h"

#include "QSettingsFixture.h"

#include <catch2/catch_test_macros.hpp>

using dish::repository::RumblePreferenceRepository;
using dish::source::RumbleEnabledStore;
using dish::test::makeSharedSettings;

TEST_CASE("a slot the user never switched rumbles", "[rumble-store]") {
    RumblePreferenceRepository repo(makeSharedSettings());
    const RumbleEnabledStore store(&repo);
    CHECK(store.isEnabled("never-switched"));
    CHECK(RumbleEnabledStore::kDefaultEnabled);
}

TEST_CASE("switching rumble off persists it and republishes the state", "[rumble-store]") {
    RumblePreferenceRepository repo(makeSharedSettings());
    RumbleEnabledStore store(&repo);

    store.setEnabled("9", false);

    REQUIRE(repo.get("9").has_value());
    CHECK_FALSE(repo.get("9")->enabled);
    CHECK_FALSE(store.state().value().at("9"));
    CHECK_FALSE(store.isEnabled("9"));
}

TEST_CASE("each slot's rumble switch survives a restart of the store", "[rumble-store]") {
    const auto settings = makeSharedSettings();
    {
        RumblePreferenceRepository repo(settings);
        RumbleEnabledStore store(&repo);
        store.setEnabled("9", false);
        store.setEnabled("sdl:1", false);
        store.setEnabled("sdl:1", true);
    }
    RumblePreferenceRepository repo(settings);
    const RumbleEnabledStore restarted(&repo);
    CHECK_FALSE(restarted.isEnabled("9"));
    CHECK(restarted.isEnabled("sdl:1"));
    CHECK(restarted.isEnabled("never-switched"));
}
