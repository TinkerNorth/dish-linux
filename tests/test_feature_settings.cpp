// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

// Coverage for dish::FeatureSettings and the pure LightbarMode <-> key/label
// mappings (Task 1.4). FeatureSettings is the "Light bar" on/off preference:
// the gate AppModel's lightbar handlers consult. Two things matter and are
// tested here:
//   * the pure mode mappings (persisted key, display label, lenient parse),
//   * that setLightbarMode persists and that lightbarFollowGame() — the
//     thread-safe gate read on the receive thread — tracks the mode.
//
// QSettings is injected as a throwaway temp-file store so the test never
// touches the real user configuration.

#include "FeatureSettings.h"

#include <catch2/catch_test_macros.hpp>

#include <QSettings>
#include <QString>
#include <QTemporaryDir>

#include <memory>

using dish::FeatureSettings;
using dish::LightbarMode;
using dish::lightbarModeFromKey;
using dish::lightbarModeLabel;
using dish::lightbarModeToKey;

namespace {

// A FeatureSettings backed by a fresh ini file inside `dir`, so each test gets
// an isolated store and a relaunch can be simulated by constructing a second
// instance over the same path.
std::unique_ptr<FeatureSettings> makeSettings(const QTemporaryDir& dir) {
    const QString path = dir.filePath(QStringLiteral("features.ini"));
    auto store = std::make_unique<QSettings>(path, QSettings::IniFormat);
    return std::make_unique<FeatureSettings>(std::move(store));
}

} // namespace

// --- Pure mappings ----------------------------------------------------------

TEST_CASE("lightbarModeToKey round-trips through lightbarModeFromKey",
          "[featuresettings][lightbar]") {
    REQUIRE(lightbarModeFromKey(lightbarModeToKey(LightbarMode::FollowGame)) ==
            LightbarMode::FollowGame);
    REQUIRE(lightbarModeFromKey(lightbarModeToKey(LightbarMode::Off)) == LightbarMode::Off);
}

TEST_CASE("lightbarModeToKey uses the dish-mac vocabulary", "[featuresettings][lightbar]") {
    // The persisted strings match dish-mac's LightbarMode rawValue so a future
    // shared sync layer sees one vocabulary.
    REQUIRE(lightbarModeToKey(LightbarMode::FollowGame) == QStringLiteral("followGame"));
    REQUIRE(lightbarModeToKey(LightbarMode::Off) == QStringLiteral("off"));
}

TEST_CASE("lightbarModeFromKey is lenient — unknown keys fall back to FollowGame",
          "[featuresettings][lightbar]") {
    // First launch (empty), a typo, or a forward-newer value all default to
    // the documented FollowGame rather than disabling the light bar.
    REQUIRE(lightbarModeFromKey(QString()) == LightbarMode::FollowGame);
    REQUIRE(lightbarModeFromKey(QStringLiteral("")) == LightbarMode::FollowGame);
    REQUIRE(lightbarModeFromKey(QStringLiteral("garbage")) == LightbarMode::FollowGame);
    REQUIRE(lightbarModeFromKey(QStringLiteral("FollowGame")) == LightbarMode::FollowGame);
    // Only the exact "off" token disables it.
    REQUIRE(lightbarModeFromKey(QStringLiteral("off")) == LightbarMode::Off);
}

TEST_CASE("lightbarModeLabel returns the user-facing strings", "[featuresettings][lightbar]") {
    REQUIRE(lightbarModeLabel(LightbarMode::FollowGame) == QStringLiteral("Follow game"));
    REQUIRE(lightbarModeLabel(LightbarMode::Off) == QStringLiteral("Off"));
}

// --- FeatureSettings behaviour ---------------------------------------------

TEST_CASE("FeatureSettings defaults to Follow game on a fresh store",
          "[featuresettings][lightbar]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    auto settings = makeSettings(dir);
    REQUIRE(settings->lightbarMode() == LightbarMode::FollowGame);
    // The hot-path gate agrees with the mode.
    REQUIRE(settings->lightbarFollowGame());
}

TEST_CASE("setLightbarMode updates the mode and the thread-safe gate",
          "[featuresettings][lightbar]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    auto settings = makeSettings(dir);

    settings->setLightbarMode(LightbarMode::Off);
    REQUIRE(settings->lightbarMode() == LightbarMode::Off);
    // lightbarFollowGame() is what the receive thread reads — it must flip.
    REQUIRE_FALSE(settings->lightbarFollowGame());

    settings->setLightbarMode(LightbarMode::FollowGame);
    REQUIRE(settings->lightbarMode() == LightbarMode::FollowGame);
    REQUIRE(settings->lightbarFollowGame());
}

TEST_CASE("setLightbarMode emits changed() only on an actual transition",
          "[featuresettings][lightbar]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    auto settings = makeSettings(dir);

    int changes = 0;
    QObject::connect(settings.get(), &FeatureSettings::changed, [&] { ++changes; });

    settings->setLightbarMode(LightbarMode::FollowGame); // already the default — no-op
    REQUIRE(changes == 0);

    settings->setLightbarMode(LightbarMode::Off); // real change
    REQUIRE(changes == 1);

    settings->setLightbarMode(LightbarMode::Off); // unchanged — no-op
    REQUIRE(changes == 1);
}

TEST_CASE("FeatureSettings persists the light-bar mode across a relaunch",
          "[featuresettings][lightbar]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());

    {
        auto settings = makeSettings(dir);
        settings->setLightbarMode(LightbarMode::Off);
    }
    // A second instance over the same ini file is a stand-in for an app
    // relaunch — it must load the persisted "Off".
    {
        auto reloaded = makeSettings(dir);
        REQUIRE(reloaded->lightbarMode() == LightbarMode::Off);
        REQUIRE_FALSE(reloaded->lightbarFollowGame());
    }
}
