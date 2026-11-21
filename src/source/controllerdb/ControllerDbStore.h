// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// ControllerDbStore — owns the database in force (docs/controller-db.md
// section 6). The bundled copy is the app's Qt resource :/controllers/v1/,
// taken from one published dbVersion; the downloaded one lives under
// $XDG_DATA_HOME/TinkerNorth/Dish/controllers/v1/<dbVersion>/, and a `current` file beside
// those folders names the one in force. A commit writes the new folder file by
// file (each written beside and renamed over, QSaveFile), then renames
// `current` over: that rename is the swap, so a failure or a crash at any step
// before it leaves the previous cache in force. Every load re-checks each
// cached file against its manifest's SHA-256 and size, and a cache that does
// not verify is ignored in favour of the bundled copy.
//
// Main thread only. The catalog it hands out is immutable and shared.

#pragma once

#include "architecture/StateSource.h"
#include "core/controllerdb/ControllerCatalog.h"
#include "core/controllerdb/DbFetch.h"
#include "source/store/ControllerDbPreferenceStore.h"

#include <QByteArray>
#include <QMap>
#include <QString>

#include <cstdint>
#include <map>
#include <optional>

namespace dish::source::controllerdb {

namespace cdb = dish::controllerdb;

// What Diagnostics shows about the database (section 6.7), as data.
struct ControllerDbStatus {
    int bundledVersion = 0;
    int activeVersion = 0;
    cdb::DbSource source = cdb::DbSource::Bundled;
    // 0 and empty when no check has run on this machine.
    std::int64_t lastCheckAtMs = 0;
    std::optional<cdb::CheckResult> lastCheckResult;
    bool checking = false;

    bool operator==(const ControllerDbStatus&) const = default;
};

// One database's files as they came: the manifest's bytes and each data file's by its path. A
// QMap, whose move cannot throw: MSVC's std::map allocates its sentinel when it is moved.
struct DbFiles {
    QByteArray manifest;
    QMap<QString, QByteArray> files;
};

inline constexpr const char* kBundledDbDir = ":/controllers/v1";

// QStandardPaths' AppLocalDataLocation + /controllers/v1 ($XDG_DATA_HOME/TinkerNorth/Dish, and
// the app's own ~/.var/app folder inside a Flatpak), "" when it resolves to nothing (no cache is
// then kept).
QString controllerDbCacheRoot();

// A database read and checked against its own manifest: every file the manifest names present,
// at its size and SHA-256, and every one decoding. Null when anything does not.
cdb::CatalogPtr catalogFromFiles(const DbFiles& files, cdb::DbSource source);

// The files of a database folder (the bundled resource or one cache folder), as they are on disk.
DbFiles readDbFolder(const QString& dir);

class ControllerDbStore : public arch::StateSource<ControllerDbStatus> {
  public:
    ControllerDbStore(source::ControllerDbPreferenceStore* prefs, QString bundledDir,
                      QString cacheRoot);

    // Reads the bundled copy and the cache and puts the newer in force.
    void load();

    cdb::CatalogPtr active() const { return active_; }
    cdb::CatalogPtr bundled() const { return bundled_; }

    // Writes a verified download into its own folder, then moves `current` onto it and puts it in
    // force. False when any write failed; the previous database then stays in force.
    bool commit(const DbFiles& files, const cdb::CatalogPtr& catalog);

    void setChecking(bool checking);
    void recordCheck(std::int64_t atMs, cdb::CheckResult result);

  private:
    cdb::CatalogPtr loadCache() const;
    void publish();

    source::ControllerDbPreferenceStore* prefs_;
    const QString bundledDir_;
    const QString cacheRoot_;
    cdb::CatalogPtr bundled_;
    cdb::CatalogPtr active_;
};

} // namespace dish::source::controllerdb
