// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// get() on an unwritten slot answers nullopt rather than a default boolean: the
// store layer above owns the default.

#include "repository/MotionPreferenceRepository.h"
#include "repository/RumblePreferenceRepository.h"

#include "QSettingsFixture.h"
#include "RepositoryContract.h"

#include <catch2/catch_test_macros.hpp>

#include <QByteArray>
#include <QString>

using dish::repository::MotionPreference;
using dish::repository::MotionPreferenceRepository;
using dish::repository::RumblePreference;
using dish::repository::RumblePreferenceRepository;
using dish::test::makeSharedSettings;

namespace {

constexpr const char* kRumbleListKey = "rumble_preferences";

} // namespace

TEST_CASE("RumblePreferenceRepository satisfies the repository contract", "[repository][rumble]") {
    dish::test::runRepositoryContract<QString, RumblePreference>(
        [] { return std::make_unique<RumblePreferenceRepository>(makeSharedSettings()); },
        [](int i) { return QStringLiteral("slot-%1").arg(i); },
        [](const QString& k) { return RumblePreference{k, (k.size() % 2) == 0}; });
}

TEST_CASE("a rumble switch is read back by a fresh repository over the same settings",
          "[rumble-pref]") {
    const auto settings = makeSharedSettings();
    {
        RumblePreferenceRepository repo(settings);
        repo.put(RumblePreference{"sdl:1", true});
        repo.put(RumblePreference{"9", false});
    }
    const RumblePreferenceRepository reopened(settings);
    REQUIRE(reopened.get("sdl:1").has_value());
    CHECK(reopened.get("sdl:1")->enabled);
    REQUIRE(reopened.get("9").has_value());
    CHECK_FALSE(reopened.get("9")->enabled);
}

// QJsonDocument answers an empty array for anything that is not an array,
// garbage included, which is what keeps a corrupt blob from reaching a row.
TEST_CASE("a garbled rumble list reads as no switches at all", "[rumble-pref]") {
    const auto settings = makeSharedSettings();
    settings->setValue(kRumbleListKey, QByteArrayLiteral("{not valid json"));
    const RumblePreferenceRepository repo(settings);
    CHECK(repo.all().empty());
    CHECK_FALSE(repo.get("anything").has_value());
}

TEST_CASE("a stored rumble element that is not a keyed row is skipped", "[rumble-pref]") {
    const auto settings = makeSharedSettings();
    settings->setValue(
        kRumbleListKey,
        QByteArrayLiteral(R"([1,{"slot":"9","on":true},{"k":"7","slot":"7","on":false}])"));
    const RumblePreferenceRepository repo(settings);
    CHECK(repo.all().size() == 1);
    REQUIRE(repo.get("7").has_value());
    CHECK_FALSE(repo.get("7")->enabled);
}

TEST_CASE("the rumble switches keep a list of their own, apart from motion's", "[rumble-pref]") {
    const auto settings = makeSharedSettings();
    MotionPreferenceRepository motion(settings);
    RumblePreferenceRepository rumble(settings);

    motion.put(MotionPreference{"9", false});
    CHECK_FALSE(rumble.get("9").has_value());

    rumble.put(RumblePreference{"9", true});
    rumble.clear();
    REQUIRE(motion.get("9").has_value());
    CHECK_FALSE(motion.get("9")->enabled);
}
