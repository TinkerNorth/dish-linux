// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "repository/AppSettings.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>
#include <QStringList>

#include <mutex>

namespace dish::repository {

namespace {

// NativeFormat, which on Unix is an INI file named `.conf`. IniFormat would be
// the same content under a `.ini` extension — a gratuitous break from every
// other app in ~/.config, and it would also make the migration below miss the
// legacy files, which QSettings wrote as `.conf`.
constexpr QSettings::Format kFormat = QSettings::NativeFormat;
constexpr QSettings::Scope kScope = QSettings::UserScope;

// The application half of the QSettings key. Lowercase to match the binary and
// the man page; the directory above it already carries the branded app id.
const char* kAppFile = "dish";

// Where the two pre-consolidation files lived. Ordered least- to most-trusted:
// a key present in both is taken from the LAST entry, because the "Dish"/"Dish"
// file is where the port actually wrote everything that matters.
struct LegacyLocation {
    const char* organization;
    const char* application;
};
constexpr LegacyLocation kLegacy[] = {
    {"TinkerNorth", "Dish"}, // whatever default-constructed after main() set the org
    {"Dish", "Dish"},        // the Windows-registry path the port carried over
};

} // namespace

QString settingsFilePath() {
    const QSettings probe(kFormat, kScope, QLatin1String(kAppId), QLatin1String(kAppFile));
    return probe.fileName();
}

std::unique_ptr<QSettings> makeSettings() {
    // Migration must be complete before the first reader observes an empty file,
    // or a store would cache a default and write it back over the migrated value.
    migrateLegacySettings();
    auto settings = std::make_unique<QSettings>(kFormat, kScope, QLatin1String(kAppId),
                                                QLatin1String(kAppFile));
    return settings;
}

void restrictSettingsPermissions() {
    const QString path = settingsFilePath();
    if (!QFileInfo::exists(path)) { return; }
    // ReadOwner|WriteOwner == 0600. QFile::setPermissions replaces the whole mode
    // rather than masking, so this also strips a group bit an earlier run left.
    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
}

void mergeSettings(QSettings& target, QSettings& source) {
    for (const QString& key : source.allKeys()) {
        // Never clobber: the new file is authoritative the moment it has an
        // opinion, so a migration cannot undo a later user change.
        if (target.contains(key)) { continue; }
        target.setValue(key, source.value(key));
    }
    target.sync();
}

void migrateLegacySettings() {
    static std::once_flag once;
    std::call_once(once, [] {
        QSettings target(kFormat, kScope, QLatin1String(kAppId), QLatin1String(kAppFile));

        for (const auto& legacy : kLegacy) {
            QSettings source(kFormat, kScope, QLatin1String(legacy.organization),
                             QLatin1String(legacy.application));
            const QString sourcePath = source.fileName();
            if (!QFileInfo::exists(sourcePath)) { continue; }

            mergeSettings(target, source);

            // Renamed rather than deleted. A migration that got something wrong is
            // then still fixable by hand, and the rename is what makes the next
            // run skip this file rather than re-reading it forever.
            QFile::rename(sourcePath, sourcePath + QLatin1String(".migrated"));
        }

        target.sync();
        restrictSettingsPermissions();
    });
}

} // namespace dish::repository
