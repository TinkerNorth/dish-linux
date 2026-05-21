// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "FeatureSettings.h"

namespace dish {

namespace {

// QSettings key for the persisted light-bar mode. The string values match
// dish-mac's LightbarMode rawValue ("followGame" / "off") so a future shared
// sync layer sees one vocabulary.
constexpr const char* kLightbarModeKey = "feature_lightbar_mode";
constexpr const char* kLightbarFollowGame = "followGame";
constexpr const char* kLightbarOff = "off";

} // namespace

QString lightbarModeToKey(LightbarMode mode) {
    switch (mode) {
    case LightbarMode::Off:
        return QString::fromLatin1(kLightbarOff);
    case LightbarMode::FollowGame:
        break;
    }
    return QString::fromLatin1(kLightbarFollowGame);
}

LightbarMode lightbarModeFromKey(const QString& key) {
    // Lenient: only the exact "off" token disables the light bar. Anything
    // else — including an empty / unknown value from a first launch or a
    // forward-newer config — falls back to the documented default.
    if (key == QLatin1String(kLightbarOff)) { return LightbarMode::Off; }
    return LightbarMode::FollowGame;
}

QString lightbarModeLabel(LightbarMode mode) {
    switch (mode) {
    case LightbarMode::Off:
        return QStringLiteral("Off");
    case LightbarMode::FollowGame:
        break;
    }
    return QStringLiteral("Follow game");
}

FeatureSettings::FeatureSettings(QObject* parent)
    : FeatureSettings(std::make_unique<QSettings>(QStringLiteral("Dish"), QStringLiteral("Dish")),
                      parent) {}

FeatureSettings::FeatureSettings(std::unique_ptr<QSettings> settings, QObject* parent)
    : QObject(parent), settings_(std::move(settings)) {
    load();
}

FeatureSettings::~FeatureSettings() = default;

void FeatureSettings::load() {
    const auto raw = settings_->value(QLatin1String(kLightbarModeKey)).toString();
    lightbarMode_ = lightbarModeFromKey(raw);
    lightbarFollowGame_.store(lightbarMode_ == LightbarMode::FollowGame, std::memory_order_relaxed);
}

void FeatureSettings::setLightbarMode(LightbarMode mode) {
    if (mode == lightbarMode_) { return; }
    lightbarMode_ = mode;
    lightbarFollowGame_.store(mode == LightbarMode::FollowGame, std::memory_order_relaxed);
    settings_->setValue(QLatin1String(kLightbarModeKey), lightbarModeToKey(mode));
    emit changed();
}

} // namespace dish
