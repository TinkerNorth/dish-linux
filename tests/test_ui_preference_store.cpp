// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The rail's collapsed state is persisted shell state, so the key NAME is
// pinned as a literal: renaming it silently re-expands the rail for every user
// on upgrade, and nothing else would fail.
//
// Every case runs against a temp INI through the injecting constructor, so no
// test here touches the user's real settings.

#include "source/store/UiPreferenceStore.h"

#include "StateSourceProbe.h"

#include <catch2/catch_test_macros.hpp>

#include <QSettings>
#include <QString>
#include <QTemporaryDir>

#include <memory>

using dish::source::UiPreferences;
using dish::source::UiPreferenceStore;
using dish::test::StateSourceProbe;

namespace {

std::unique_ptr<UiPreferenceStore> makeStore(const QString& iniPath) {
    return std::make_unique<UiPreferenceStore>(
        std::make_unique<QSettings>(iniPath, QSettings::IniFormat));
}

} // namespace

TEST_CASE("ui preference store: the key name is a persisted schema", "[ui][ui-prefs]") {
    CHECK(QString::fromLatin1(UiPreferenceStore::kKeyRailCollapsed) ==
          QStringLiteral("ui_rail_collapsed"));
}

TEST_CASE("ui preference store: the rail starts expanded", "[ui][ui-prefs]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const auto store = makeStore(dir.filePath(QStringLiteral("ui.ini")));
    CHECK_FALSE(store->railCollapsed());
    // A fresh file must read back as the struct's own defaults, so the two
    // cannot drift apart.
    CHECK(store->state().value() == UiPreferences{});
}

TEST_CASE("ui preference store: a set persists and survives a reopen", "[ui][ui-prefs]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString ini = dir.filePath(QStringLiteral("ui.ini"));
    {
        const auto store = makeStore(ini);
        store->setRailCollapsed(true);
        CHECK(store->railCollapsed());
    }
    QSettings raw(ini, QSettings::IniFormat);
    CHECK(raw.value(QLatin1String(UiPreferenceStore::kKeyRailCollapsed)).toBool());

    const auto reopened = makeStore(ini);
    CHECK(reopened->railCollapsed());
}

TEST_CASE("ui preference store: a seeded file is read at construction", "[ui][ui-prefs]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString ini = dir.filePath(QStringLiteral("ui.ini"));
    {
        QSettings seed(ini, QSettings::IniFormat);
        seed.setValue(QLatin1String(UiPreferenceStore::kKeyRailCollapsed), true);
        seed.sync();
    }
    const auto store = makeStore(ini);
    CHECK(store->railCollapsed());
    CHECK(store->state().value().railCollapsed);
}

TEST_CASE("ui preference store: a set republishes exactly once", "[ui][ui-prefs]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const auto store = makeStore(dir.filePath(QStringLiteral("ui.ini")));
    StateSourceProbe<UiPreferences> probe(store->state());
    // The one emission is the current value replayed to the new subscriber.
    CHECK(probe.count() == 1);

    store->setRailCollapsed(true);
    CHECK(probe.count() == 2);
    CHECK(probe.latest().railCollapsed);
}

TEST_CASE("ui preference store: a repeat set does not re-emit", "[ui][ui-prefs]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const auto store = makeStore(dir.filePath(QStringLiteral("ui.ini")));
    StateSourceProbe<UiPreferences> probe(store->state());

    store->setRailCollapsed(true);
    CHECK(probe.count() == 2);
    // Idempotent, so a subscriber that sets from its own callback cannot loop.
    store->setRailCollapsed(true);
    CHECK(probe.count() == 2);

    // Expanding again is a change like any other.
    store->setRailCollapsed(false);
    CHECK(probe.count() == 3);
    CHECK_FALSE(probe.latest().railCollapsed);
    store->setRailCollapsed(false);
    CHECK(probe.count() == 3);
}

TEST_CASE("ui preference store: a set that changes nothing writes nothing", "[ui][ui-prefs]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString ini = dir.filePath(QStringLiteral("ui.ini"));
    {
        const auto store = makeStore(ini);
        store->setRailCollapsed(false); // already false
    }
    // The distinct-until-changed guard returns before the write, so the key is
    // not even created: an untouched preference stays absent from the file.
    QSettings raw(ini, QSettings::IniFormat);
    CHECK_FALSE(raw.contains(QLatin1String(UiPreferenceStore::kKeyRailCollapsed)));
}

TEST_CASE("ui preference store: the preference slice compares field-wise", "[ui][ui-prefs]") {
    // This is the distinct-until-changed predicate: a field dropped from it
    // stops republishing, silently.
    UiPreferences a;
    UiPreferences b;
    CHECK(a == b);
    b.railCollapsed = true;
    CHECK(a != b);
}
