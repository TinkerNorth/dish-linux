// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The two reactive update preferences and the one imperative bookkeeping key.
// The key NAMES are pinned as literals on purpose: they are a persisted schema,
// and renaming one silently resets a user's choice, so a rename has to break
// this test.
//
// Every case runs against a temp INI through the injecting constructor, so no
// test here touches the user's real settings.

#include "source/store/UpdatePreferenceStore.h"

#include "StateSourceProbe.h"

#include <catch2/catch_test_macros.hpp>

#include <QSettings>
#include <QString>
#include <QTemporaryDir>

#include <memory>

using dish::source::kKeyUpdatesLastCheckUtcMs;
using dish::source::UpdatePreferences;
using dish::source::UpdatePreferenceStore;
using dish::test::StateSourceProbe;

namespace {

std::unique_ptr<UpdatePreferenceStore> makeStore(const QString& iniPath) {
    return std::make_unique<UpdatePreferenceStore>(
        std::make_unique<QSettings>(iniPath, QSettings::IniFormat));
}

} // namespace

TEST_CASE("update preference store: the key names are a persisted schema",
          "[update][update-prefs]") {
    CHECK(QString::fromLatin1(UpdatePreferenceStore::kKeyChecksEnabled) ==
          QStringLiteral("updates_check_enabled"));
    CHECK(QString::fromLatin1(UpdatePreferenceStore::kKeySkippedVersion) ==
          QStringLiteral("updates_skipped_version"));
    // Written by the checker, never part of the reactive slice.
    CHECK(QString::fromLatin1(kKeyUpdatesLastCheckUtcMs) ==
          QStringLiteral("updates_last_check_utc_ms"));
}

TEST_CASE("update preference store: checks default on, nothing skipped", "[update][update-prefs]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const auto store = makeStore(dir.filePath(QStringLiteral("prefs.ini")));
    CHECK(store->checksEnabled());
    CHECK(store->skippedVersion().isEmpty());
}

TEST_CASE("update preference store: a set persists and survives a reopen",
          "[update][update-prefs]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString ini = dir.filePath(QStringLiteral("prefs.ini"));
    {
        const auto store = makeStore(ini);
        store->setChecksEnabled(false);
        store->setSkippedVersion(QStringLiteral("0.4.0"));
    }
    QSettings raw(ini, QSettings::IniFormat);
    CHECK(raw.value(QLatin1String(UpdatePreferenceStore::kKeyChecksEnabled)).toBool() == false);
    CHECK(raw.value(QLatin1String(UpdatePreferenceStore::kKeySkippedVersion)).toString() ==
          QStringLiteral("0.4.0"));

    const auto reopened = makeStore(ini);
    CHECK_FALSE(reopened->checksEnabled());
    CHECK(reopened->skippedVersion() == QStringLiteral("0.4.0"));
}

TEST_CASE("update preference store: a seeded file is read at construction",
          "[update][update-prefs]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString ini = dir.filePath(QStringLiteral("prefs.ini"));
    {
        QSettings seed(ini, QSettings::IniFormat);
        seed.setValue(QLatin1String(UpdatePreferenceStore::kKeyChecksEnabled), false);
        seed.setValue(QLatin1String(UpdatePreferenceStore::kKeySkippedVersion),
                      QStringLiteral("1.2.3"));
        seed.sync();
    }
    const auto store = makeStore(ini);
    CHECK_FALSE(store->checksEnabled());
    CHECK(store->skippedVersion() == QStringLiteral("1.2.3"));
}

TEST_CASE("update preference store: a repeat set does not re-emit", "[update][update-prefs]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const auto store = makeStore(dir.filePath(QStringLiteral("prefs.ini")));
    StateSourceProbe<UpdatePreferences> probe(store->state());

    store->setChecksEnabled(false);
    CHECK(probe.count() == 2); // the replayed initial value, then the change
    CHECK_FALSE(probe.latest().checksEnabled);

    // Idempotent, so the checker cannot loop through its own subscription.
    store->setChecksEnabled(false);
    CHECK(probe.count() == 2);

    store->setSkippedVersion(QStringLiteral("2.0.0"));
    CHECK(probe.count() == 3);
    CHECK(probe.latest().skippedVersion == QStringLiteral("2.0.0"));
    store->setSkippedVersion(QStringLiteral("2.0.0"));
    CHECK(probe.count() == 3);
}
