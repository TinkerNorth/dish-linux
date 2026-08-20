// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

// QSettings is injected as a throwaway temp-file store so the test never touches
// the real user configuration.

#include "FeatureSettings.h"

#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
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

// One ini file per `dir`, so a second instance over the same dir stands in for
// an app relaunch.
std::unique_ptr<FeatureSettings> makeSettings(const QTemporaryDir& dir) {
    const QString path = dir.filePath(QStringLiteral("features.ini"));
    auto store = std::make_unique<QSettings>(path, QSettings::IniFormat);
    return std::make_unique<FeatureSettings>(std::move(store));
}

} // namespace

TEST_CASE("lightbarModeToKey round-trips through lightbarModeFromKey",
          "[featuresettings][lightbar]") {
    REQUIRE(lightbarModeFromKey(lightbarModeToKey(LightbarMode::FollowGame)) ==
            LightbarMode::FollowGame);
    REQUIRE(lightbarModeFromKey(lightbarModeToKey(LightbarMode::Off)) == LightbarMode::Off);
}

TEST_CASE("lightbarModeToKey uses the dish-mac vocabulary", "[featuresettings][lightbar]") {
    // Same tokens as dish-mac's LightbarMode rawValue, so a future shared sync
    // layer sees one vocabulary.
    REQUIRE(lightbarModeToKey(LightbarMode::FollowGame) == QStringLiteral("followGame"));
    REQUIRE(lightbarModeToKey(LightbarMode::Off) == QStringLiteral("off"));
}

TEST_CASE("lightbarModeFromKey is lenient -- unknown keys fall back to FollowGame",
          "[featuresettings][lightbar]") {
    REQUIRE(lightbarModeFromKey(QString()) == LightbarMode::FollowGame);
    REQUIRE(lightbarModeFromKey(QStringLiteral("")) == LightbarMode::FollowGame);
    REQUIRE(lightbarModeFromKey(QStringLiteral("garbage")) == LightbarMode::FollowGame);
    REQUIRE(lightbarModeFromKey(QStringLiteral("FollowGame")) == LightbarMode::FollowGame);
    // Only the exact "off" token disables it.
    REQUIRE(lightbarModeFromKey(QStringLiteral("off")) == LightbarMode::Off);
}

TEST_CASE("lightbarModeLabel returns the user-facing strings", "[featuresettings][lightbar]") {
    // The label routes through QCoreApplication::translate, so pin it against the
    // same call rather than the English literal: it must pass under every
    // bundled translator.
    REQUIRE(lightbarModeLabel(LightbarMode::FollowGame) ==
            QCoreApplication::translate("FeatureSettings", "Follow game"));
    REQUIRE(lightbarModeLabel(LightbarMode::Off) ==
            QCoreApplication::translate("FeatureSettings", "Off"));
}

TEST_CASE("FeatureSettings defaults to Follow game on a fresh store",
          "[featuresettings][lightbar]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    auto settings = makeSettings(dir);
    REQUIRE(settings->lightbarMode() == LightbarMode::FollowGame);
    REQUIRE(settings->lightbarFollowGame());
}

TEST_CASE("setLightbarMode updates the mode and the thread-safe gate",
          "[featuresettings][lightbar]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    auto settings = makeSettings(dir);

    settings->setLightbarMode(LightbarMode::Off);
    REQUIRE(settings->lightbarMode() == LightbarMode::Off);
    // lightbarFollowGame() is the receive-thread read; it has to flip too.
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

    settings->setLightbarMode(LightbarMode::FollowGame); // already the default
    REQUIRE(changes == 0);

    settings->setLightbarMode(LightbarMode::Off);
    REQUIRE(changes == 1);

    settings->setLightbarMode(LightbarMode::Off);
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
    {
        auto reloaded = makeSettings(dir);
        REQUIRE(reloaded->lightbarMode() == LightbarMode::Off);
        REQUIRE_FALSE(reloaded->lightbarFollowGame());
    }
}
