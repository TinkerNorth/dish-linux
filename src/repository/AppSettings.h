// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// AppSettings — the one place that decides where Dish's settings live on disk.
//
// Every store and repository used to default-construct QSettings("Dish","Dish"),
// which is a Windows registry path (HKCU\Software\Dish\Dish) inherited verbatim
// from the dish-windows port. On Linux that resolves to ~/.config/Dish/Dish.conf,
// which is neither the documented path nor an XDG-shaped one, and it left the app
// writing TWO files: this one for everything that mattered, and
// ~/.config/TinkerNorth/Dish.conf for whatever happened to default-construct
// QSettings after main() set the organization.
//
// The file is now $XDG_CONFIG_HOME/com.tinkernorth.Dish/dish.conf, keyed by the
// same reverse-DNS app id as the .desktop entry, the AppStream metainfo and the
// Flatpak. `migrateLegacySettings` folds both old files into it once.

#pragma once

#include <QString>

#include <memory>

class QSettings;

namespace dish::repository {

// The reverse-DNS app id. Same string as packaging/com.tinkernorth.Dish.desktop.
inline constexpr const char* kAppId = "com.tinkernorth.Dish";

// A fresh QSettings over the app's one config file. Each caller owns its
// instance; QSettings keeps concurrent instances over one file coherent.
std::unique_ptr<QSettings> makeSettings();

// Absolute path of that file. Exposed for the migration, the tests and for
// anything that needs to tell the user where their settings actually are.
QString settingsFilePath();

// Copies ~/.config/Dish/Dish.conf and ~/.config/TinkerNorth/Dish.conf into the
// new file, then renames each away with a `.migrated` suffix so a second run is
// a no-op and a bad migration is still recoverable by hand.
//
// Never overwrites a key the new file already has: re-running after the user has
// changed something must not resurrect the old value. Runs at most once per
// process, and is safe to call before any QSettings is constructed.
void migrateLegacySettings();

// Copies every key `source` has and `target` does not into `target`. The
// no-clobber rule is the whole contract: migration must never undo a change the
// user made after the new file already existed. Exposed for the tests, which
// drive it over two temporary files rather than the real ones.
void mergeSettings(QSettings& target, QSettings& source);

// Tightens the config file to 0600. It holds the satellite pairing keys, and
// QSettings creates it 0644-minus-umask — group- and world-readable on a stock
// Ubuntu. Idempotent, and silently does nothing if the file does not exist yet.
void restrictSettingsPermissions();

} // namespace dish::repository
