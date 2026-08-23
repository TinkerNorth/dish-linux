// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// AppViewModel is a thin forwarder over already-tested stores, so what is pinned
// here is the mapping layer it re-projects (qml/AppSettingsMaps) plus the store
// round-trips driven THROUGH those maps.

#include "qml/AppSettingsMaps.h"

#include "repository/DeadzoneRepository.h"
#include "repository/MotionPreferenceRepository.h"
#include "source/store/CrashReportingStore.h"
#include "source/store/MotionEnabledStore.h"
#include "source/store/OnboardingPreferenceStore.h"
#include "source/store/ThemePreferenceStore.h"
#include "UI/licenses/LicenseManifest.h"

#include "QSettingsFixture.h"

#include <catch2/catch_test_macros.hpp>

#include <QByteArray>
#include <QDir>
#include <QSettings>
#include <QString>
#include <QUuid>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>

#include <memory>

using dish::qml::deadzoneRowFor;
using dish::qml::kDefaultDeadzoneStickFlat;
using dish::qml::kDefaultDeadzoneTriggerFlat;
using dish::qml::keepAwakeModeFromInt;
using dish::qml::keepAwakeModeToInt;
using dish::qml::keepAwakeReachToken;
using dish::qml::licenseRows;
using dish::qml::themeModeFromInt;
using dish::qml::themeModeToInt;
using dish::reducer::KeepAwakeMode;
using dish::reducer::KeepAwakeReach;
using dish::repository::DeadzoneRepository;
using dish::repository::MotionPreferenceRepository;
using dish::source::CrashReportingStore;
using dish::source::MotionEnabledStore;
using dish::source::OnboardingPreferenceStore;
using dish::source::ThemeMode;
using dish::source::ThemePreferenceStore;
using dish::test::makeSharedSettings;

namespace {

// A unique temp INI, never the real HKCU registry.
std::unique_ptr<QSettings> uniqueIniSettings(const char* tag) {
    const QString path = QDir::tempPath() + QStringLiteral("/dish-%1-").arg(tag) +
                         QUuid::createUuid().toString(QUuid::WithoutBraces) +
                         QStringLiteral(".ini");
    return std::make_unique<QSettings>(path, QSettings::IniFormat);
}

} // namespace

TEST_CASE("themeMode int contract is Light=0 Dark=1 System=2", "[appvm][theme]") {
    REQUIRE(themeModeToInt(ThemeMode::Light) == 0);
    REQUIRE(themeModeToInt(ThemeMode::Dark) == 1);
    REQUIRE(themeModeToInt(ThemeMode::System) == 2);
    REQUIRE(themeModeFromInt(0) == ThemeMode::Light);
    REQUIRE(themeModeFromInt(1) == ThemeMode::Dark);
    REQUIRE(themeModeFromInt(2) == ThemeMode::System);
}

TEST_CASE("themeMode int round-trips for all three modes", "[appvm][theme]") {
    for (auto m : {ThemeMode::Light, ThemeMode::Dark, ThemeMode::System}) {
        REQUIRE(themeModeFromInt(themeModeToInt(m)) == m);
    }
}

TEST_CASE("themeModeFromInt is lenient -- out-of-range falls back to System", "[appvm][theme]") {
    REQUIRE(themeModeFromInt(-1) == ThemeMode::System);
    REQUIRE(themeModeFromInt(3) == ThemeMode::System);
    REQUIRE(themeModeFromInt(99) == ThemeMode::System);
}

TEST_CASE("themeMode get/set round-trips through the store via the int maps", "[appvm][theme]") {
    ThemePreferenceStore store(uniqueIniSettings("theme"));
    REQUIRE(themeModeToInt(store.mode()) == 2); // fresh store defaults to System

    store.setMode(themeModeFromInt(1)); // "Dark"
    REQUIRE(store.mode() == ThemeMode::Dark);
    REQUIRE(themeModeToInt(store.mode()) == 1);

    store.setMode(themeModeFromInt(0)); // "Light"
    REQUIRE(themeModeToInt(store.mode()) == 0);
}

TEST_CASE("keepAwakeMode int contract is Off=0 Active=1 Connected=2", "[appvm][keepawake]") {
    // The SettingsPage option order. It happens to match the enum here, but the
    // QML contract is the int, so it is pinned rather than assumed.
    REQUIRE(keepAwakeModeToInt(KeepAwakeMode::Off) == 0);
    REQUIRE(keepAwakeModeToInt(KeepAwakeMode::WhileControllerActive) == 1);
    REQUIRE(keepAwakeModeToInt(KeepAwakeMode::WhileConnected) == 2);
    REQUIRE(keepAwakeModeFromInt(0) == KeepAwakeMode::Off);
    REQUIRE(keepAwakeModeFromInt(1) == KeepAwakeMode::WhileControllerActive);
    REQUIRE(keepAwakeModeFromInt(2) == KeepAwakeMode::WhileConnected);
}

TEST_CASE("keepAwakeMode int round-trips for all three modes", "[appvm][keepawake]") {
    for (auto m : {KeepAwakeMode::Off, KeepAwakeMode::WhileControllerActive,
                   KeepAwakeMode::WhileConnected}) {
        REQUIRE(keepAwakeModeFromInt(keepAwakeModeToInt(m)) == m);
    }
}

TEST_CASE("keepAwakeModeFromInt is lenient -- out-of-range falls back to the timed mode",
          "[appvm][keepawake]") {
    // Matches keepAwakeModeFromKey: a bad value must never pin the machine
    // awake, so it lands on the timed mode rather than WhileConnected.
    REQUIRE(keepAwakeModeFromInt(-1) == KeepAwakeMode::WhileControllerActive);
    REQUIRE(keepAwakeModeFromInt(3) == KeepAwakeMode::WhileControllerActive);
    REQUIRE(keepAwakeModeFromInt(99) == KeepAwakeMode::WhileControllerActive);
}

TEST_CASE("keepAwakeReachToken names the three reaches for QML", "[appvm][keepawake]") {
    REQUIRE(keepAwakeReachToken(KeepAwakeReach::None) == QStringLiteral("off"));
    REQUIRE(keepAwakeReachToken(KeepAwakeReach::System) == QStringLiteral("system"));
    REQUIRE(keepAwakeReachToken(KeepAwakeReach::SystemAndDisplay) == QStringLiteral("display"));
}

TEST_CASE("crash-reporting toggle forwards through the store", "[appvm][crash]") {
    CrashReportingStore store(uniqueIniSettings("crash"));
    REQUIRE(store.enabled() == true); // opt-out: default ON

    store.setEnabled(false);
    REQUIRE(store.enabled() == false);
    store.setEnabled(true);
    REQUIRE(store.enabled() == true);
}

TEST_CASE("deadzoneRowFor seeds the default profile for an unset device", "[appvm][deadzone]") {
    DeadzoneRepository repo(makeSharedSettings());
    MotionPreferenceRepository motionRepo(makeSharedSettings());
    MotionEnabledStore motion(&motionRepo);

    const auto row = deadzoneRowFor(QStringLiteral("pad-1"), QStringLiteral("DualSense"),
                                    /*hasGyro=*/true, &repo, &motion);
    CHECK(row.value(QStringLiteral("id")).toString() == QStringLiteral("pad-1"));
    CHECK(row.value(QStringLiteral("name")).toString() == QStringLiteral("DualSense"));
    CHECK(row.value(QStringLiteral("hasGyro")).toBool() == true);
    CHECK(row.value(QStringLiteral("stickFlat")).toInt() == kDefaultDeadzoneStickFlat);
    CHECK(row.value(QStringLiteral("triggerFlat")).toInt() == kDefaultDeadzoneTriggerFlat);
    // Motion defaults ON for an untouched device.
    CHECK(row.value(QStringLiteral("forwardMotion")).toBool() == true);
}

TEST_CASE("deadzoneRowFor reflects a stored override + a disabled motion toggle",
          "[appvm][deadzone]") {
    DeadzoneRepository repo(makeSharedSettings());
    MotionPreferenceRepository motionRepo(makeSharedSettings());
    MotionEnabledStore motion(&motionRepo);

    repo.setDeadzones(QStringLiteral("pad-1"), {5000, 40});
    motion.setEnabled(std::string("pad-1"), false);

    const auto row = deadzoneRowFor(QStringLiteral("pad-1"), QStringLiteral("Pad"),
                                    /*hasGyro=*/true, &repo, &motion);
    CHECK(row.value(QStringLiteral("stickFlat")).toInt() == 5000);
    CHECK(row.value(QStringLiteral("triggerFlat")).toInt() == 40);
    CHECK(row.value(QStringLiteral("forwardMotion")).toBool() == false);
}

TEST_CASE("deadzone/motion are keyed by device id and stay independent", "[appvm][deadzone]") {
    DeadzoneRepository repo(makeSharedSettings());
    MotionPreferenceRepository motionRepo(makeSharedSettings());
    MotionEnabledStore motion(&motionRepo);

    repo.setDeadzones(QStringLiteral("a"), {1111, 11});
    motion.setEnabled(std::string("b"), false);

    const auto a = deadzoneRowFor(QStringLiteral("a"), QString(), true, &repo, &motion);
    const auto b = deadzoneRowFor(QStringLiteral("b"), QString(), true, &repo, &motion);

    CHECK(a.value(QStringLiteral("stickFlat")).toInt() == 1111);
    CHECK(a.value(QStringLiteral("forwardMotion")).toBool() == true);
    CHECK(b.value(QStringLiteral("stickFlat")).toInt() == kDefaultDeadzoneStickFlat);
    CHECK(b.value(QStringLiteral("forwardMotion")).toBool() == false);
}

TEST_CASE("deadzoneRowFor tolerates null stores (pure defaults)", "[appvm][deadzone]") {
    const auto row =
        deadzoneRowFor(QStringLiteral("x"), QStringLiteral("X"), false, nullptr, nullptr);
    CHECK(row.value(QStringLiteral("stickFlat")).toInt() == kDefaultDeadzoneStickFlat);
    CHECK(row.value(QStringLiteral("triggerFlat")).toInt() == kDefaultDeadzoneTriggerFlat);
    CHECK(row.value(QStringLiteral("forwardMotion")).toBool() ==
          MotionEnabledStore::kDefaultEnabled);
}

TEST_CASE("licenseRows maps a manifest to {name,version,license,url}", "[appvm][licenses]") {
    const auto manifest = dish::ui::parseLicenseManifest(QByteArray(R"({
        "libraries":[
          {"name":"Qt 6","version":"6.7.3","url":"https://qt.io",
           "licenses":[{"name":"LGPL-3.0","url":"https://l/lgpl"}]},
          {"group":"io.lib","artifact":"thing","version":"1.0"}
        ]
    })"));

    const QVariantList rows = licenseRows(manifest);
    REQUIRE(rows.size() == 2);

    const auto first = rows.at(0).toMap();
    CHECK(first.value(QStringLiteral("name")).toString() == QStringLiteral("Qt 6"));
    CHECK(first.value(QStringLiteral("version")).toString() == QStringLiteral("6.7.3"));
    CHECK(first.value(QStringLiteral("license")).toString() == QStringLiteral("LGPL-3.0"));
    // Click-url precedence: licenses[0].url wins over the entry url.
    CHECK(first.value(QStringLiteral("url")).toString() == QStringLiteral("https://l/lgpl"));

    // No name -> the display name falls back to group:artifact.
    const auto second = rows.at(1).toMap();
    CHECK(second.value(QStringLiteral("name")).toString() == QStringLiteral("io.lib:thing"));
    CHECK(second.value(QStringLiteral("license")).toString().isEmpty());
    CHECK(second.value(QStringLiteral("url")).toString().isEmpty());
}

TEST_CASE("licenseRows drops an unnamed entry", "[appvm][licenses]") {
    const auto manifest = dish::ui::parseLicenseManifest(QByteArray(R"({
        "libraries":[ {"version":"9"} ]
    })"));
    CHECK(licenseRows(manifest).isEmpty());
}

TEST_CASE("onboardingNeeded reflects the store and clears on markWelcomeCompleted",
          "[appvm][onboarding]") {
    OnboardingPreferenceStore store(uniqueIniSettings("onb"));
    // onboardingNeeded is !welcomeCompleted().
    REQUIRE(store.welcomeCompleted() == false);

    store.markWelcomeCompleted();
    REQUIRE(store.welcomeCompleted() == true);

    store.markWelcomeCompleted();
    REQUIRE(store.welcomeCompleted() == true);
}
