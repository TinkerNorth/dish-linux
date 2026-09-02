// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// get() on an unwritten slot answers nullopt rather than a default boolean: the
// store layer above owns the per-direction defaults. One class serves both
// directions keyed apart, so isolation between the two lists is asserted here.

#include "repository/AudioPreferenceRepository.h"

#include "QSettingsFixture.h"
#include "RepositoryContract.h"

#include <catch2/catch_test_macros.hpp>

#include <QString>

using dish::repository::AudioPreference;
using dish::repository::AudioPreferenceRepository;
using dish::test::makeSharedSettings;

TEST_CASE("AudioPreferenceRepository satisfies the repository contract", "[repository][audio]") {
    dish::test::runRepositoryContract<QString, AudioPreference>(
        [] {
            return std::make_unique<AudioPreferenceRepository>(QStringLiteral("mic_preferences"),
                                                               makeSharedSettings());
        },
        [](int i) { return QStringLiteral("slot-%1").arg(i); },
        [](const QString& k) { return AudioPreference{k, (k.size() % 2) == 0}; });
}

TEST_CASE("audio toggles round-trip with durability", "[audio-pref]") {
    auto store = makeSharedSettings();
    {
        AudioPreferenceRepository repo(QStringLiteral("mic_preferences"), store);
        repo.put(AudioPreference{"sdl:1", true});
        repo.put(AudioPreference{"9", false});
    }
    // A fresh repo over the same store rules out an in-memory cache.
    AudioPreferenceRepository repo2(QStringLiteral("mic_preferences"), store);
    REQUIRE(repo2.get("sdl:1").has_value());
    CHECK(repo2.get("sdl:1")->enabled == true);
    REQUIRE(repo2.get("9").has_value());
    CHECK(repo2.get("9")->enabled == false);
}

TEST_CASE("get on a slot that was never written returns nullopt, not a default", "[audio-pref]") {
    AudioPreferenceRepository repo(QStringLiteral("speaker_preferences"), makeSharedSettings());
    CHECK_FALSE(repo.get("never-written").has_value());
}

TEST_CASE("corrupt audio JSON falls back to empty, does not crash", "[audio-pref]") {
    auto store = makeSharedSettings();
    store->setValue(QStringLiteral("mic_preferences"), QByteArrayLiteral("{not valid json"));
    AudioPreferenceRepository repo(QStringLiteral("mic_preferences"), store);
    CHECK(repo.all().empty());
    CHECK_FALSE(repo.get("anything").has_value());
}

TEST_CASE("the two direction keys are fully isolated", "[audio-pref]") {
    auto store = makeSharedSettings();
    AudioPreferenceRepository mic(QStringLiteral("mic_preferences"), store);
    AudioPreferenceRepository speaker(QStringLiteral("speaker_preferences"), store);

    mic.put(AudioPreference{"9", true});
    CHECK_FALSE(speaker.get("9").has_value());

    speaker.put(AudioPreference{"9", false});
    speaker.clear();
    // Clearing one direction leaves the other's list intact.
    REQUIRE(mic.get("9").has_value());
    CHECK(mic.get("9")->enabled == true);
    CHECK_FALSE(speaker.get("9").has_value());
}

TEST_CASE("audio put with the same slot replaces in place, list never grows", "[audio-pref]") {
    AudioPreferenceRepository repo(QStringLiteral("mic_preferences"), makeSharedSettings());
    repo.put(AudioPreference{"sdl:1", true});
    repo.put(AudioPreference{"sdl:1", false});
    repo.put(AudioPreference{"sdl:1", true});
    CHECK(repo.all().size() == 1);
    CHECK(repo.get("sdl:1")->enabled == true);
}
