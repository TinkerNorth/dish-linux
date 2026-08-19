// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The theme-mode StateSource: owns the persisted dark/light/system choice and
// republishes it reactively.
//
// This class only DERIVES the mode. Resolving System against the OS preference
// and re-theming the live application is an effect, and lives in
// composer::ThemeController. Keeping them apart is what stops the persisted
// value and the rendered palette from drifting.
//
// The storage values and key are a cross-client schema shared with the other
// Dish clients, so renaming one is a migration. Unknown or empty resolves to
// System.

#pragma once

#include "architecture/StateSource.h"

#include <QString>

#include <memory>

class QSettings;

namespace dish::source {

// The three appearance modes the picker offers. SYSTEM follows the OS; LIGHT /
// DARK pin it.
enum class ThemeMode { System, Light, Dark };

// Pure mappings between ThemeMode and its persisted storage token / display
// label. Free functions (no QObject) so tests pin them without a store.
// `themeModeFromStorage` is lenient: null / empty / unknown / forward-newer ->
// System (matches android's fromStorageValue default).
QString themeModeToStorage(ThemeMode mode);
ThemeMode themeModeFromStorage(const QString& value);
QString themeModeLabel(ThemeMode mode);

class ThemePreferenceStore : public arch::StateSource<ThemeMode> {
  public:
    // Persisted key + store name — verbatim from android for cross-client schema
    // continuity.
    static constexpr const char* kKeyThemeMode = "theme_mode";

    // The documented default for a fresh install: follow the OS.
    static constexpr ThemeMode kDefault = ThemeMode::System;

    // Production ctor: a QSettings under the app org (the same store the other
    // user_preferences live in). Test ctor: inject a throwaway store.
    ThemePreferenceStore();
    explicit ThemePreferenceStore(std::unique_ptr<QSettings> settings);
    ~ThemePreferenceStore() override;

    ThemeMode mode() const { return state().value(); }

    // Persist the mode + republish it. Distinct-until-changed: a redundant
    // setMode is a no-op (the Observable suppresses the re-emit), matching the
    // android-test "emit only on a real transition" contract.
    void setMode(ThemeMode mode);

  private:
    static ThemeMode readInitial(QSettings& settings);

    std::unique_ptr<QSettings> settings_;
};

} // namespace dish::source
