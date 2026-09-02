// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// MicEnabledStore / SpeakerEnabledStore (source/store/AudioEnabledStore.h): the
// same behaviour contract as MotionEnabledStore, plus the pair's one load-
// bearing difference — the defaults. Mic OFF is the privacy stance (capture is
// opt-in per binding), speaker ON is the working default, and both are asserted
// here so neither can be "tidied" into the other.

#include "repository/AudioPreferenceRepository.h"
#include "source/store/AudioEnabledStore.h"

#include "QSettingsFixture.h"

#include <catch2/catch_test_macros.hpp>

#include <memory>

using dish::repository::AudioPreference;
using dish::repository::AudioPreferenceRepository;
using dish::source::MicEnabledStore;
using dish::source::SpeakerEnabledStore;
using dish::test::makeSharedSettings;

namespace {

std::unique_ptr<AudioPreferenceRepository>
seededRepo(const QString& listKey, std::initializer_list<AudioPreference> prefs) {
    auto repo = std::make_unique<AudioPreferenceRepository>(listKey, makeSharedSettings());
    for (const auto& p : prefs) { repo->put(p); }
    return repo;
}

} // namespace

TEST_CASE("mic defaults OFF and speaker ON for an unwritten slot", "[audio-store]") {
    auto micRepo = seededRepo(QStringLiteral("mic_preferences"), {});
    auto spkRepo = seededRepo(QStringLiteral("speaker_preferences"), {});
    MicEnabledStore mic(micRepo.get());
    SpeakerEnabledStore speaker(spkRepo.get());

    CHECK_FALSE(mic.isEnabled("never-toggled"));
    CHECK(speaker.isEnabled("never-toggled"));
    // The class constants the UI seeds from must agree with the behaviour.
    CHECK_FALSE(MicEnabledStore::kDefaultEnabled);
    CHECK(SpeakerEnabledStore::kDefaultEnabled);
    CHECK_FALSE(mic.defaultEnabled());
    CHECK(speaker.defaultEnabled());
}

TEST_CASE("hydrates state from the repository on construction", "[audio-store]") {
    auto repo = seededRepo(QStringLiteral("mic_preferences"),
                           {AudioPreference{"sdl:1", true}, AudioPreference{"9", false}});
    MicEnabledStore store(repo.get());

    const auto& snapshot = store.state().value();
    REQUIRE(snapshot.size() == 2);
    CHECK(snapshot.at("sdl:1") == true);
    CHECK(snapshot.at("9") == false);
}

TEST_CASE("setEnabled persists to the repository AND republishes state", "[audio-store]") {
    auto repo = seededRepo(QStringLiteral("mic_preferences"), {});
    MicEnabledStore store(repo.get());

    store.setEnabled("9", true);

    REQUIRE(repo->get("9").has_value());
    CHECK(repo->get("9")->enabled == true);
    CHECK(store.state().value().at("9") == true);
    CHECK(store.isEnabled("9"));
}

TEST_CASE("an explicit off survives even where the default is on", "[audio-store]") {
    auto repo = seededRepo(QStringLiteral("speaker_preferences"), {AudioPreference{"9", false}});
    SpeakerEnabledStore store(repo.get());
    CHECK_FALSE(store.isEnabled("9"));
    CHECK(store.isEnabled("other")); // the default still answers for the rest
}

TEST_CASE("forget removes the entry from state AND from the repository", "[audio-store]") {
    auto repo = seededRepo(QStringLiteral("mic_preferences"), {AudioPreference{"9", true}});
    MicEnabledStore store(repo.get());
    CHECK(store.isEnabled("9"));

    store.forget("9");

    CHECK_FALSE(repo->get("9").has_value());
    // Absent means the mic default (off) applies again — the forget must not
    // leave a microphone silently enabled.
    CHECK_FALSE(store.isEnabled("9"));
    const auto snapshot = store.state().value();
    CHECK(snapshot.find("9") == snapshot.end());
}

TEST_CASE("the two directions never share state through their repos", "[audio-store]") {
    // Same slot id, same QSettings file, different list keys: a mic toggle
    // must not move the speaker's answer.
    const auto settings = makeSharedSettings();
    AudioPreferenceRepository micRepo(QStringLiteral("mic_preferences"), settings);
    AudioPreferenceRepository spkRepo(QStringLiteral("speaker_preferences"), settings);
    MicEnabledStore mic(&micRepo);
    SpeakerEnabledStore speaker(&spkRepo);

    mic.setEnabled("9", true);
    speaker.setEnabled("9", false);

    CHECK(mic.isEnabled("9"));
    CHECK_FALSE(speaker.isEnabled("9"));
    CHECK(micRepo.get("9")->enabled);
    CHECK_FALSE(spkRepo.get("9")->enabled);
}
