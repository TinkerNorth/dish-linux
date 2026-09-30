// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Driven through the real store, the way the binding pages flip the switch, so a
// stop is pinned to the flip that caused it.

#include "composer/RumbleSwitchController.h"
#include "repository/RumblePreferenceRepository.h"
#include "source/store/RumbleEnabledStore.h"

#include "QSettingsFixture.h"

#include <catch2/catch_test_macros.hpp>

#include <QSettings>
#include <QString>
#include <QStringList>

#include <memory>

using dish::composer::RumbleSwitchController;
using dish::repository::RumblePreference;
using dish::repository::RumblePreferenceRepository;
using dish::source::RumbleEnabledStore;
using dish::test::makeSharedSettings;

namespace {

// The store as the app builds it, over settings a test may seed first, with every
// stop the controller asks for recorded in order.
struct SwitchesUnderControl {
    explicit SwitchesUnderControl(std::shared_ptr<QSettings> seeded = makeSharedSettings())
        : repo(std::move(seeded)) {
        controller.start();
    }

    void record(const QString& slotId) { stopped.append(slotId); }

    RumblePreferenceRepository repo;
    RumbleEnabledStore store{&repo};
    QStringList stopped;
    RumbleSwitchController controller{store.state(),
                                      [this](const QString& slotId) { record(slotId); }};
};

} // namespace

TEST_CASE("switching a slot's rumble off stops that slot's motors", "[rumble][switch]") {
    SwitchesUnderControl f;
    f.store.setEnabled("9", false);
    CHECK(f.stopped == QStringList{"9"});
}

TEST_CASE("switching rumble on stops nothing, and off again stops once more", "[rumble][switch]") {
    SwitchesUnderControl f;
    f.store.setEnabled("9", true);
    CHECK(f.stopped.isEmpty());

    f.store.setEnabled("9", false);
    f.store.setEnabled("9", true);
    CHECK(f.stopped == QStringList{"9"});

    f.store.setEnabled("9", false);
    CHECK(f.stopped == QStringList({"9", "9"}));
}

TEST_CASE("another slot's switch leaves a slot already off alone", "[rumble][switch]") {
    SwitchesUnderControl f;
    f.store.setEnabled("9", false);
    f.store.setEnabled("7", false);
    CHECK(f.stopped == QStringList({"9", "7"}));
}

TEST_CASE("a switch already off when the app starts stops nothing", "[rumble][switch]") {
    const auto settings = makeSharedSettings();
    RumblePreferenceRepository(settings).put(RumblePreference{"9", false});

    SwitchesUnderControl f(settings);

    CHECK_FALSE(f.store.isEnabled("9"));
    CHECK(f.stopped.isEmpty());
}
