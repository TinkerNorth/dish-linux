// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The consolidation of the two pre-XDG config files into
// $XDG_CONFIG_HOME/com.tinkernorth.Dish/dish.conf.
//
// The no-clobber rule is the part worth pinning: the migration runs on every
// launch until the legacy files are renamed away, and a merge that overwrote
// would silently revert a setting the user changed after upgrading.
//
// Driven over temporary files, never the real ones — `mergeSettings` is the
// seam precisely so these tests never touch a developer's own config.

#include "repository/AppSettings.h"

#include <QSettings>
#include <QTemporaryDir>

#include <catch2/catch_test_macros.hpp>

using namespace dish::repository;

TEST_CASE("merge copies keys the target has never heard of") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    QSettings target(dir.filePath("target.conf"), QSettings::IniFormat);
    QSettings source(dir.filePath("source.conf"), QSettings::IniFormat);

    source.setValue("deviceId", "abc123");
    source.setValue("satellite_list", "[]");
    source.sync();

    mergeSettings(target, source);

    REQUIRE(target.value("deviceId").toString() == "abc123");
    REQUIRE(target.value("satellite_list").toString() == "[]");
}

TEST_CASE("merge never overwrites a key the target already has") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    QSettings target(dir.filePath("target.conf"), QSettings::IniFormat);
    QSettings source(dir.filePath("source.conf"), QSettings::IniFormat);

    // The user re-paired after the upgrade, so the new file's key is the live
    // one. Letting the stale legacy value win would break every later request.
    target.setValue("deviceId", "new-identity");
    source.setValue("deviceId", "stale-identity");
    source.setValue("wifi_list", "carried-over");
    source.sync();

    mergeSettings(target, source);

    REQUIRE(target.value("deviceId").toString() == "new-identity");
    // Untouched keys still migrate: no-clobber is per key, not per file.
    REQUIRE(target.value("wifi_list").toString() == "carried-over");
}

TEST_CASE("merge preserves group-qualified keys") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    QSettings target(dir.filePath("target.conf"), QSettings::IniFormat);
    QSettings source(dir.filePath("source.conf"), QSettings::IniFormat);

    // The legacy shared-key namespace is a real INI group, and allKeys() vends
    // it as "group/key". Losing the prefix would collide entries from different
    // namespaces, which is the one thing SettingsKeys.h exists to prevent.
    source.setValue("wifi_shared_key/wifi:10.0.0.2:9876", "deadbeef");
    source.sync();

    mergeSettings(target, source);

    REQUIRE(target.value("wifi_shared_key/wifi:10.0.0.2:9876").toString() == "deadbeef");
}

TEST_CASE("merging an empty source leaves the target alone") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    QSettings target(dir.filePath("target.conf"), QSettings::IniFormat);
    QSettings source(dir.filePath("source.conf"), QSettings::IniFormat);

    target.setValue("keep", "me");
    mergeSettings(target, source);

    REQUIRE(target.value("keep").toString() == "me");
    REQUIRE(target.allKeys().size() == 1);
}

TEST_CASE("the settings path is the reverse-DNS app id under XDG config") {
    // Same string as the .desktop entry, the AppStream metainfo and the Flatpak
    // id; a drift here splits the config again, which is what this whole change
    // set exists to undo.
    const QString path = settingsFilePath();
    REQUIRE(path.contains("com.tinkernorth.Dish"));
    // NativeFormat on Unix, so `.conf` and not `.ini`.
    REQUIRE(path.endsWith(".conf"));
}
