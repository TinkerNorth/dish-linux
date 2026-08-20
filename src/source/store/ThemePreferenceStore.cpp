// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "source/store/ThemePreferenceStore.h"

#include <QCoreApplication>
#include <QSettings>

namespace dish::source {

namespace {
// Storage tokens — kept verbatim from android (cross-client schema). Add new
// values, never rename existing ones.
constexpr const char* kStorageSystem = "system";
constexpr const char* kStorageLight = "light";
constexpr const char* kStorageDark = "dark";
} // namespace

QString themeModeToStorage(ThemeMode mode) {
    switch (mode) {
    case ThemeMode::Light:
        return QString::fromLatin1(kStorageLight);
    case ThemeMode::Dark:
        return QString::fromLatin1(kStorageDark);
    case ThemeMode::System:
        break;
    }
    return QString::fromLatin1(kStorageSystem);
}

ThemeMode themeModeFromStorage(const QString& value) {
    // Exact-token match; everything else (empty, typo, forward-newer) -> System,
    // matching android's fromStorageValue(else -> SYSTEM).
    if (value == QLatin1String(kStorageLight)) { return ThemeMode::Light; }
    if (value == QLatin1String(kStorageDark)) { return ThemeMode::Dark; }
    return ThemeMode::System;
}

QString themeModeLabel(ThemeMode mode) {
    // Locale-aware: routed through translate so a localized run picks the bundled
    // form. The context literal must be passed inline (lupdate cannot resolve a
    // context held in a variable). Mirrors android's settings_appearance_* ids.
    switch (mode) {
    case ThemeMode::Light:
        return QCoreApplication::translate("dish::source::ThemePreferenceStore", "Light");
    case ThemeMode::Dark:
        return QCoreApplication::translate("dish::source::ThemePreferenceStore", "Dark");
    case ThemeMode::System:
        break;
    }
    return QCoreApplication::translate("dish::source::ThemePreferenceStore", "System");
}

ThemeMode ThemePreferenceStore::readInitial(QSettings& settings) {
    return themeModeFromStorage(settings.value(QLatin1String(kKeyThemeMode)).toString());
}

ThemePreferenceStore::ThemePreferenceStore()
    : ThemePreferenceStore(
          std::make_unique<QSettings>(QStringLiteral("Dish"), QStringLiteral("Dish"))) {}

ThemePreferenceStore::ThemePreferenceStore(std::unique_ptr<QSettings> settings)
    : arch::StateSource<ThemeMode>(readInitial(*settings)), settings_(std::move(settings)) {}

ThemePreferenceStore::~ThemePreferenceStore() = default;

void ThemePreferenceStore::setMode(ThemeMode mode) {
    settings_->setValue(QLatin1String(kKeyThemeMode), themeModeToStorage(mode));
    // setState publishes only on a real transition (Observable ==-compare), so a
    // redundant setMode does not re-emit — the android distinct-until-changed
    // contract.
    setState(mode);
}

} // namespace dish::source
