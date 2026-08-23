// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// All three keys are persisted schema, so their NAMES are pinned as literals
// here: renaming keep_awake_mode silently reverts every user to the timed mode,
// and renaming keep_awake_display silently un-does the "keep my screen on"
// opt-in. Neither would fail anything else. The clamp is pinned on both edges —
// a hand-edited INI is the one path that can put an out-of-range idle window in
// front of the reducer.
//
// Every case runs against a temp INI through the injecting constructor, so no
// test here touches the user's real settings.

#include "source/store/KeepAwakePreferenceStore.h"

#include "StateSourceProbe.h"

#include <catch2/catch_test_macros.hpp>

#include <QSettings>
#include <QString>
#include <QTemporaryDir>

#include <memory>

using dish::reducer::KeepAwakeMode;
using dish::reducer::KeepAwakePreferences;
using dish::reducer::kKeepAwakeDefaultTimeoutMinutes;
using dish::reducer::kKeepAwakeMaxTimeoutMinutes;
using dish::reducer::kKeepAwakeMinTimeoutMinutes;
using dish::source::KeepAwakePreferenceStore;
using dish::test::StateSourceProbe;

namespace {

std::unique_ptr<KeepAwakePreferenceStore> makeStore(const QString& iniPath) {
    return std::make_unique<KeepAwakePreferenceStore>(
        std::make_unique<QSettings>(iniPath, QSettings::IniFormat));
}

} // namespace

TEST_CASE("keep-awake preference store: the key names are a persisted schema",
          "[settings][keepawake]") {
    CHECK(QString::fromLatin1(KeepAwakePreferenceStore::kKeyMode) ==
          QStringLiteral("keep_awake_mode"));
    CHECK(QString::fromLatin1(KeepAwakePreferenceStore::kKeyTimeoutMinutes) ==
          QStringLiteral("keep_awake_timeout_minutes"));
    CHECK(QString::fromLatin1(KeepAwakePreferenceStore::kKeyDisplay) ==
          QStringLiteral("keep_awake_display"));
}

TEST_CASE("keep-awake preference store: a fresh store is the timed mode, machine only",
          "[settings][keepawake]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const auto store = makeStore(dir.filePath(QStringLiteral("keepawake.ini")));
    CHECK(store->mode() == KeepAwakeMode::WhileControllerActive);
    CHECK(store->idleTimeoutMinutes() == kKeepAwakeDefaultTimeoutMinutes);
    CHECK_FALSE(store->keepDisplayAwake());
    // A fresh file must read back as the struct's own defaults, so the two
    // cannot drift apart.
    CHECK(store->state().value() == KeepAwakePreferences{});
}

TEST_CASE("keep-awake preference store: the mode persists as its stable token",
          "[settings][keepawake]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString ini = dir.filePath(QStringLiteral("keepawake.ini"));
    {
        const auto store = makeStore(ini);
        store->setMode(KeepAwakeMode::WhileConnected);
        CHECK(store->mode() == KeepAwakeMode::WhileConnected);
    }
    QSettings raw(ini, QSettings::IniFormat);
    // The token, not the enum ordinal: reordering the enum must not repoint
    // every user's stored mode.
    CHECK(raw.value(QLatin1String(KeepAwakePreferenceStore::kKeyMode)).toString() ==
          QStringLiteral("connected"));

    const auto reopened = makeStore(ini);
    CHECK(reopened->mode() == KeepAwakeMode::WhileConnected);
}

TEST_CASE("keep-awake preference store: mode Off survives a reopen", "[settings][keepawake]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString ini = dir.filePath(QStringLiteral("keepawake.ini"));
    {
        const auto store = makeStore(ini);
        store->setMode(KeepAwakeMode::Off);
    }
    QSettings raw(ini, QSettings::IniFormat);
    CHECK(raw.value(QLatin1String(KeepAwakePreferenceStore::kKeyMode)).toString() ==
          QStringLiteral("off"));
    CHECK(makeStore(ini)->mode() == KeepAwakeMode::Off);
}

TEST_CASE("keep-awake preference store: the idle window survives a reopen",
          "[settings][keepawake]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString ini = dir.filePath(QStringLiteral("keepawake.ini"));
    {
        const auto store = makeStore(ini);
        store->setIdleTimeoutMinutes(20);
        CHECK(store->idleTimeoutMinutes() == 20);
    }
    QSettings raw(ini, QSettings::IniFormat);
    CHECK(raw.value(QLatin1String(KeepAwakePreferenceStore::kKeyTimeoutMinutes)).toInt() == 20);
    CHECK(makeStore(ini)->idleTimeoutMinutes() == 20);
}

TEST_CASE("keep-awake preference store: the display opt-in survives a reopen",
          "[settings][keepawake]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString ini = dir.filePath(QStringLiteral("keepawake.ini"));
    {
        const auto store = makeStore(ini);
        store->setKeepDisplayAwake(true);
        CHECK(store->keepDisplayAwake());
    }
    QSettings raw(ini, QSettings::IniFormat);
    CHECK(raw.value(QLatin1String(KeepAwakePreferenceStore::kKeyDisplay)).toBool());
    CHECK(makeStore(ini)->keepDisplayAwake());
}

TEST_CASE("keep-awake preference store: the idle window is clamped on write",
          "[settings][keepawake]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString ini = dir.filePath(QStringLiteral("keepawake.ini"));
    const auto store = makeStore(ini);

    store->setIdleTimeoutMinutes(0);
    CHECK(store->idleTimeoutMinutes() == kKeepAwakeMinTimeoutMinutes);
    // The clamped value is what lands on disk, so a reopen cannot resurrect it.
    QSettings low(ini, QSettings::IniFormat);
    CHECK(low.value(QLatin1String(KeepAwakePreferenceStore::kKeyTimeoutMinutes)).toInt() ==
          kKeepAwakeMinTimeoutMinutes);

    store->setIdleTimeoutMinutes(9999);
    CHECK(store->idleTimeoutMinutes() == kKeepAwakeMaxTimeoutMinutes);
    QSettings high(ini, QSettings::IniFormat);
    CHECK(high.value(QLatin1String(KeepAwakePreferenceStore::kKeyTimeoutMinutes)).toInt() ==
          kKeepAwakeMaxTimeoutMinutes);
}

TEST_CASE("keep-awake preference store: a hand-edited out-of-range window is clamped on read",
          "[settings][keepawake]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString ini = dir.filePath(QStringLiteral("keepawake.ini"));
    {
        QSettings seed(ini, QSettings::IniFormat);
        seed.setValue(QLatin1String(KeepAwakePreferenceStore::kKeyTimeoutMinutes), 100000);
        seed.sync();
    }
    CHECK(makeStore(ini)->idleTimeoutMinutes() == kKeepAwakeMaxTimeoutMinutes);

    {
        QSettings seed(ini, QSettings::IniFormat);
        seed.setValue(QLatin1String(KeepAwakePreferenceStore::kKeyTimeoutMinutes), -5);
        seed.sync();
    }
    CHECK(makeStore(ini)->idleTimeoutMinutes() == kKeepAwakeMinTimeoutMinutes);
}

TEST_CASE("keep-awake preference store: an unknown mode token reads back as the default",
          "[settings][keepawake]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString ini = dir.filePath(QStringLiteral("keepawake.ini"));
    {
        QSettings seed(ini, QSettings::IniFormat);
        // A downgrade or a hand-edit must never leave the machine pinned awake.
        seed.setValue(QLatin1String(KeepAwakePreferenceStore::kKeyMode),
                      QStringLiteral("forever-and-ever"));
        seed.sync();
    }
    CHECK(makeStore(ini)->mode() == KeepAwakeMode::WhileControllerActive);

    {
        QSettings seed(ini, QSettings::IniFormat);
        seed.setValue(QLatin1String(KeepAwakePreferenceStore::kKeyMode), QString());
        seed.sync();
    }
    CHECK(makeStore(ini)->mode() == KeepAwakeMode::WhileControllerActive);
}

TEST_CASE("keep-awake preference store: a seeded file is read at construction",
          "[settings][keepawake]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString ini = dir.filePath(QStringLiteral("keepawake.ini"));
    {
        QSettings seed(ini, QSettings::IniFormat);
        seed.setValue(QLatin1String(KeepAwakePreferenceStore::kKeyMode), QStringLiteral("off"));
        seed.setValue(QLatin1String(KeepAwakePreferenceStore::kKeyTimeoutMinutes), 42);
        seed.setValue(QLatin1String(KeepAwakePreferenceStore::kKeyDisplay), true);
        seed.sync();
    }
    const auto store = makeStore(ini);
    CHECK(store->state().value().mode == KeepAwakeMode::Off);
    CHECK(store->state().value().idleTimeoutMinutes == 42);
    CHECK(store->state().value().keepDisplayAwake);
}

TEST_CASE("keep-awake preference store: each setter republishes exactly once",
          "[settings][keepawake]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const auto store = makeStore(dir.filePath(QStringLiteral("keepawake.ini")));
    StateSourceProbe<KeepAwakePreferences> probe(store->state());
    // The one emission is the current value replayed to the new subscriber.
    CHECK(probe.count() == 1);

    store->setMode(KeepAwakeMode::WhileConnected);
    CHECK(probe.count() == 2);
    CHECK(probe.latest().mode == KeepAwakeMode::WhileConnected);

    store->setIdleTimeoutMinutes(30);
    CHECK(probe.count() == 3);
    CHECK(probe.latest().idleTimeoutMinutes == 30);

    store->setKeepDisplayAwake(true);
    CHECK(probe.count() == 4);
    CHECK(probe.latest().keepDisplayAwake);
}

TEST_CASE("keep-awake preference store: a repeat set does not re-emit", "[settings][keepawake]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const auto store = makeStore(dir.filePath(QStringLiteral("keepawake.ini")));
    StateSourceProbe<KeepAwakePreferences> probe(store->state());

    // Already the defaults, so none of these change anything at all.
    store->setMode(KeepAwakeMode::WhileControllerActive);
    store->setIdleTimeoutMinutes(kKeepAwakeDefaultTimeoutMinutes);
    store->setKeepDisplayAwake(false);
    CHECK(probe.count() == 1);

    store->setMode(KeepAwakeMode::Off);
    CHECK(probe.count() == 2);
    // Idempotent, so a subscriber that sets from its own callback cannot loop.
    store->setMode(KeepAwakeMode::Off);
    CHECK(probe.count() == 2);

    store->setKeepDisplayAwake(true);
    CHECK(probe.count() == 3);
    store->setKeepDisplayAwake(true);
    CHECK(probe.count() == 3);
}

TEST_CASE("keep-awake preference store: two out-of-range writes clamping alike do not re-emit",
          "[settings][keepawake]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const auto store = makeStore(dir.filePath(QStringLiteral("keepawake.ini")));
    StateSourceProbe<KeepAwakePreferences> probe(store->state());

    store->setIdleTimeoutMinutes(-100);
    CHECK(probe.count() == 2);
    CHECK(probe.latest().idleTimeoutMinutes == kKeepAwakeMinTimeoutMinutes);
    // The comparison happens after the clamp, so a slider dragged past the end
    // does not re-emit for every step beyond it.
    store->setIdleTimeoutMinutes(0);
    CHECK(probe.count() == 2);
}

TEST_CASE("keep-awake preference store: setting one field leaves the others untouched",
          "[settings][keepawake]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString ini = dir.filePath(QStringLiteral("keepawake.ini"));
    const auto store = makeStore(ini);

    store->setKeepDisplayAwake(true);
    CHECK(store->mode() == KeepAwakeMode::WhileControllerActive); // still the default
    CHECK(store->idleTimeoutMinutes() == kKeepAwakeDefaultTimeoutMinutes);

    store->setIdleTimeoutMinutes(11);
    CHECK(store->keepDisplayAwake());
    store->setMode(KeepAwakeMode::WhileConnected);
    CHECK(store->idleTimeoutMinutes() == 11);
    CHECK(store->keepDisplayAwake());
}

TEST_CASE("keep-awake preference store: a relaunch sees the previous run's writes",
          "[settings][keepawake]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString ini = dir.filePath(QStringLiteral("keepawake.ini"));
    {
        const auto first = makeStore(ini);
        first->setMode(KeepAwakeMode::WhileConnected);
        first->setIdleTimeoutMinutes(45);
        first->setKeepDisplayAwake(true);
    }
    const auto second = makeStore(ini);
    CHECK(second->mode() == KeepAwakeMode::WhileConnected);
    CHECK(second->idleTimeoutMinutes() == 45);
    CHECK(second->keepDisplayAwake());
}
