// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Both keys are persisted schema, so their NAMES are pinned as literals here:
// renaming background_run_enabled silently re-opts every user out of running in
// the background, and renaming background_notice_shown re-announces the notice
// to everyone who has already dismissed it once. Neither would fail anything
// else.
//
// Every case runs against a temp INI through the injecting constructor, so no
// test here touches the user's real settings.

#include "source/store/BackgroundPreferenceStore.h"

#include "StateSourceProbe.h"

#include <catch2/catch_test_macros.hpp>

#include <QSettings>
#include <QString>
#include <QTemporaryDir>

#include <memory>

using dish::source::BackgroundPreferences;
using dish::source::BackgroundPreferenceStore;
using dish::test::StateSourceProbe;

namespace {

std::unique_ptr<BackgroundPreferenceStore> makeStore(const QString& iniPath) {
    return std::make_unique<BackgroundPreferenceStore>(
        std::make_unique<QSettings>(iniPath, QSettings::IniFormat));
}

} // namespace

TEST_CASE("background preference store: the key names are a persisted schema",
          "[settings][background]") {
    CHECK(QString::fromLatin1(BackgroundPreferenceStore::kKeyRunInBackground) ==
          QStringLiteral("background_run_enabled"));
    CHECK(QString::fromLatin1(BackgroundPreferenceStore::kKeyNoticeShown) ==
          QStringLiteral("background_notice_shown"));
}

TEST_CASE("background preference store: a fresh store runs in the background",
          "[settings][background]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const auto store = makeStore(dir.filePath(QStringLiteral("background.ini")));
    CHECK(store->runInBackground() == BackgroundPreferenceStore::kDefaultRunInBackground);
    CHECK(store->runInBackground());
    CHECK_FALSE(store->noticeShown());
    // A fresh file must read back as the struct's own defaults, so the two
    // cannot drift apart.
    CHECK(store->state().value() == BackgroundPreferences{});
}

TEST_CASE("background preference store: the run preference survives a reopen",
          "[settings][background]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString ini = dir.filePath(QStringLiteral("background.ini"));
    {
        const auto store = makeStore(ini);
        store->setRunInBackground(false);
        CHECK_FALSE(store->runInBackground());
    }
    QSettings raw(ini, QSettings::IniFormat);
    CHECK_FALSE(raw.value(QLatin1String(BackgroundPreferenceStore::kKeyRunInBackground)).toBool());

    const auto reopened = makeStore(ini);
    CHECK_FALSE(reopened->runInBackground());
}

TEST_CASE("background preference store: the notice flag survives a reopen",
          "[settings][background]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString ini = dir.filePath(QStringLiteral("background.ini"));
    {
        const auto store = makeStore(ini);
        store->setNoticeShown(true);
        CHECK(store->noticeShown());
    }
    QSettings raw(ini, QSettings::IniFormat);
    CHECK(raw.value(QLatin1String(BackgroundPreferenceStore::kKeyNoticeShown)).toBool());

    // This is what stops a restart re-announcing the background notice.
    const auto reopened = makeStore(ini);
    CHECK(reopened->noticeShown());
}

TEST_CASE("background preference store: a seeded file is read at construction",
          "[settings][background]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString ini = dir.filePath(QStringLiteral("background.ini"));
    {
        QSettings seed(ini, QSettings::IniFormat);
        seed.setValue(QLatin1String(BackgroundPreferenceStore::kKeyRunInBackground), false);
        seed.setValue(QLatin1String(BackgroundPreferenceStore::kKeyNoticeShown), true);
        seed.sync();
    }
    const auto store = makeStore(ini);
    CHECK_FALSE(store->state().value().runInBackground);
    CHECK(store->state().value().noticeShown);
}

TEST_CASE("background preference store: a run-preference change republishes exactly once",
          "[settings][background]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const auto store = makeStore(dir.filePath(QStringLiteral("background.ini")));
    StateSourceProbe<BackgroundPreferences> probe(store->state());
    // The one emission is the current value replayed to the new subscriber.
    CHECK(probe.count() == 1);

    store->setRunInBackground(false);
    CHECK(probe.count() == 2);
    CHECK_FALSE(probe.latest().runInBackground);
}

TEST_CASE("background preference store: a repeat run-preference set does not re-emit",
          "[settings][background]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const auto store = makeStore(dir.filePath(QStringLiteral("background.ini")));
    StateSourceProbe<BackgroundPreferences> probe(store->state());

    // Already the default, so this changes nothing at all.
    store->setRunInBackground(true);
    CHECK(probe.count() == 1);

    store->setRunInBackground(false);
    CHECK(probe.count() == 2);
    // Idempotent, so a subscriber that sets from its own callback cannot loop.
    store->setRunInBackground(false);
    CHECK(probe.count() == 2);
}

TEST_CASE("background preference store: a notice set republishes exactly once, then stops",
          "[settings][background]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const auto store = makeStore(dir.filePath(QStringLiteral("background.ini")));
    StateSourceProbe<BackgroundPreferences> probe(store->state());

    store->setNoticeShown(true);
    CHECK(probe.count() == 2);
    CHECK(probe.latest().noticeShown);
    // The notice is spent once; every later close re-asserts the same flag.
    store->setNoticeShown(true);
    CHECK(probe.count() == 2);
}

TEST_CASE("background preference store: setting one field leaves the other untouched",
          "[settings][background]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString ini = dir.filePath(QStringLiteral("background.ini"));
    const auto store = makeStore(ini);

    store->setNoticeShown(true);
    CHECK(store->runInBackground()); // still the default
    store->setRunInBackground(false);
    CHECK(store->noticeShown()); // still set

    const auto reopened = makeStore(ini);
    CHECK_FALSE(reopened->runInBackground());
    CHECK(reopened->noticeShown());
}

TEST_CASE("background preference store: the preference slice compares field-wise",
          "[settings][background]") {
    // This is the distinct-until-changed predicate: a field dropped from it
    // stops republishing, silently.
    BackgroundPreferences a;
    BackgroundPreferences b;
    CHECK(a == b);
    b.runInBackground = !b.runInBackground;
    CHECK(a != b);

    BackgroundPreferences c;
    c.noticeShown = !c.noticeShown;
    CHECK(a != c);
}
