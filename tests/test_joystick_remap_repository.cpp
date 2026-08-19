// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Per-VID:PID raw-joystick remaps live under their own "joystick_remaps"
// QSettings key, deliberately disjoint from "usb_path_choices"; the two stores
// co-tenant one prefs file and must never read each other's blobs.

#include "source/store/JoystickRemapStore.h"
#include "source/store/UsbPathPreferenceStore.h"

#include "QSettingsFixture.h"
#include "RepositoryContract.h"
#include "StateSourceProbe.h"

#include <catch2/catch_test_macros.hpp>

#include <memory>

using dish::input::JoystickRemap;
using dish::input::RemapButton;
using dish::input::TriggerSource;
using dish::input::TriggerSourceKind;
using dish::reducer::PathChoice;
using dish::source::JoystickRemapEntry;
using dish::source::joystickRemapKeyFor;
using dish::source::JoystickRemapMap;
using dish::source::JoystickRemapRepository;
using dish::source::JoystickRemapStore;
using dish::source::UsbPathPreferenceRepository;
using dish::source::UsbPathPreferenceStore;
using dish::test::makeSharedSettings;
using dish::test::StateSourceProbe;

namespace {

constexpr int kVidXbox = 0x045E, kPidXbox = 0x028E;
constexpr int kVidDs4 = 0x054C, kPidDs4 = 0x05C4;

// Varies by seed because the contract's overwrite section needs
// value(key(0)) != value(key(1)).
JoystickRemap makeRemap(int seed) {
    JoystickRemap r{};
    r.leftStickX = seed % 4;
    r.rightStickX = (seed + 1) % 4;
    r.invertLeftY = (seed % 2) == 0;
    r.leftTrigger = TriggerSource{TriggerSourceKind::Button, seed + 2};
    r.buttons[static_cast<int>(RemapButton::A)] = (seed + 1) % 4;
    r.buttons[static_cast<int>(RemapButton::B)] = seed % 4;
    r.hatIndex = seed % 2;
    r.useAdaptiveTriggers = false;
    return r;
}

} // namespace

TEST_CASE("JoystickRemapRepository satisfies the contract", "[repository][joyremap]") {
    dish::test::runRepositoryContract<QString, JoystickRemapEntry>(
        [&] { return std::make_unique<JoystickRemapRepository>(makeSharedSettings()); },
        [](int i) { return QStringLiteral("%1:0001").arg(i, 4, 16, QLatin1Char('0')); },
        [](const QString& k) {
            // A seed derived from the key keeps value(key) deterministic.
            const int seed = k.left(4).toInt(nullptr, 16);
            return JoystickRemapEntry{k, makeRemap(seed)};
        });
}

TEST_CASE("setRemap then remapFor round-trips per vid and pid", "[joyremap]") {
    JoystickRemapRepository repo(makeSharedSettings());
    JoystickRemapStore store(&repo);

    const auto remap = makeRemap(3);
    store.setRemap(kVidXbox, kPidXbox, remap);

    REQUIRE(store.remapFor(kVidXbox, kPidXbox).has_value());
    CHECK(*store.remapFor(kVidXbox, kPidXbox) == remap);
    // nullopt is not "no remap stored" to the caller, it means "default layout".
    CHECK_FALSE(store.remapFor(kVidDs4, kPidDs4).has_value());
}

TEST_CASE("remaps survive into a fresh store over the same prefs", "[joyremap]") {
    auto settings = makeSharedSettings();
    const auto remap = makeRemap(2);
    {
        JoystickRemapRepository repo(settings);
        JoystickRemapStore store(&repo);
        store.setRemap(kVidXbox, kPidXbox, remap);
    }
    JoystickRemapRepository reopenedRepo(settings);
    JoystickRemapStore reopened(&reopenedRepo);

    REQUIRE(reopened.remapFor(kVidXbox, kPidXbox).has_value());
    CHECK(*reopened.remapFor(kVidXbox, kPidXbox) == remap);
}

TEST_CASE("a full custom remap survives a reopen field-for-field", "[joyremap]") {
    auto settings = makeSharedSettings();
    JoystickRemap r{};
    r.leftStickX = 5;
    r.leftStickY = 6;
    r.rightStickX = 7;
    r.rightStickY = 8;
    r.invertLeftY = false;
    r.invertRightY = false;
    r.leftTrigger = TriggerSource{TriggerSourceKind::Button, 11};
    r.rightTrigger = TriggerSource{TriggerSourceKind::Axis, 9};
    for (int i = 0; i < dish::input::kRemapButtonCount; ++i) { r.buttons[i] = i + 1; }
    r.hatIndex = 2;
    r.useAdaptiveRightStick = false;
    r.useAdaptiveTriggers = false;
    {
        JoystickRemapRepository repo(settings);
        JoystickRemapStore store(&repo);
        store.setRemap(kVidDs4, kPidDs4, r);
    }
    JoystickRemapRepository repo2(settings);
    JoystickRemapStore store2(&repo2);
    REQUIRE(store2.remapFor(kVidDs4, kPidDs4).has_value());
    CHECK(*store2.remapFor(kVidDs4, kPidDs4) == r);
}

TEST_CASE("clearRemap removes only the targeted pad", "[joyremap]") {
    JoystickRemapRepository repo(makeSharedSettings());
    JoystickRemapStore store(&repo);
    store.setRemap(kVidXbox, kPidXbox, makeRemap(1));
    store.setRemap(kVidDs4, kPidDs4, makeRemap(2));

    store.clearRemap(kVidXbox, kPidXbox);

    CHECK_FALSE(store.remapFor(kVidXbox, kPidXbox).has_value());
    REQUIRE(store.remapFor(kVidDs4, kPidDs4).has_value());
    CHECK(*store.remapFor(kVidDs4, kPidDs4) == makeRemap(2));
}

TEST_CASE("setRemap to the current value does not re-emit", "[joyremap]") {
    JoystickRemapRepository repo(makeSharedSettings());
    JoystickRemapStore store(&repo);
    const auto remap = makeRemap(1);
    store.setRemap(kVidXbox, kPidXbox, remap);

    StateSourceProbe<JoystickRemapMap> probe(store.state());
    REQUIRE(probe.count() == 1); // the eager initial emission

    store.setRemap(kVidXbox, kPidXbox, remap); // identical → short-circuits

    CHECK(probe.count() == 1); // no re-emit
}

TEST_CASE("a corrupt remaps blob falls back to an empty map without crashing", "[joyremap]") {
    auto settings = makeSharedSettings();
    settings->setValue(QLatin1String(JoystickRemapRepository::kRemapsKey),
                       QByteArray("{not valid json"));

    JoystickRemapRepository repo(settings);
    JoystickRemapStore store(&repo);

    CHECK_FALSE(store.remapFor(kVidXbox, kPidXbox).has_value());
}

TEST_CASE("a partially-garbled entry falls back to the default remap", "[joyremap]") {
    auto settings = makeSharedSettings();
    // Forward-compat rule: an absent or wrong-typed field reads its default, and
    // the key still surfaces rather than being dropped.
    settings->setValue(QLatin1String(JoystickRemapRepository::kRemapsKey),
                       QByteArray(R"({"045e:028e":{"lsx":"notanumber","unknown":42}})"));

    JoystickRemapRepository repo(settings);
    JoystickRemapStore store(&repo);

    auto got = store.remapFor(kVidXbox, kPidXbox);
    REQUIRE(got.has_value());
    CHECK(*got == JoystickRemap{}); // every garbled/unknown field defaulted
}

TEST_CASE("a non-object entry value decodes to the default remap", "[joyremap]") {
    auto settings = makeSharedSettings();
    settings->setValue(QLatin1String(JoystickRemapRepository::kRemapsKey),
                       QByteArray(R"({"045e:028e":"future"})"));

    JoystickRemapRepository repo(settings);
    JoystickRemapStore store(&repo);

    auto got = store.remapFor(kVidXbox, kPidXbox);
    REQUIRE(got.has_value());
    CHECK(*got == JoystickRemap{});
}

TEST_CASE("a remap never reads back from usb_path_choices and vice versa", "[joyremap]") {
    auto settings = makeSharedSettings(); // ONE shared prefs co-tenanted by both

    JoystickRemapRepository remapRepo(settings);
    JoystickRemapStore remapStore(&remapRepo);
    remapStore.setRemap(kVidXbox, kPidXbox, makeRemap(3));

    UsbPathPreferenceRepository pathRepo(settings);
    UsbPathPreferenceStore pathStore(&pathRepo);
    pathStore.setChoice(kVidXbox, kPidXbox, PathChoice::Direct);

    CHECK(remapStore.remapFor(kVidXbox, kPidXbox).has_value());
    CHECK(pathStore.choiceFor(kVidXbox, kPidXbox).has_value());

    CHECK(QString::fromLatin1(JoystickRemapRepository::kRemapsKey) !=
          QString::fromLatin1(UsbPathPreferenceRepository::kChoicesKey));

    auto settings2 = makeSharedSettings();
    settings2->setValue(QLatin1String(UsbPathPreferenceRepository::kChoicesKey),
                        QByteArray(R"({"045e:028e":"direct"})"));
    JoystickRemapRepository onlyPathSeeded(settings2);
    JoystickRemapStore onlyPathStore(&onlyPathSeeded);
    CHECK_FALSE(onlyPathStore.remapFor(kVidXbox, kPidXbox).has_value());

    auto settings3 = makeSharedSettings();
    JoystickRemapRepository seedRemap(settings3);
    JoystickRemapStore seedRemapStore(&seedRemap);
    seedRemapStore.setRemap(kVidXbox, kPidXbox, makeRemap(1));
    UsbPathPreferenceRepository pathOver(settings3);
    UsbPathPreferenceStore pathOverStore(&pathOver);
    CHECK_FALSE(pathOverStore.choiceFor(kVidXbox, kPidXbox).has_value());
}

TEST_CASE("the key shape matches the seeded vid:pid", "[joyremap]") {
    CHECK(joystickRemapKeyFor(kVidXbox, kPidXbox) == QStringLiteral("045e:028e"));
}
