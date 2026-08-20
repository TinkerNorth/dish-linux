// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "source/store/UsbPathPreferenceStore.h"

#include "QSettingsFixture.h"
#include "RepositoryContract.h"
#include "StateSourceProbe.h"

#include <catch2/catch_test_macros.hpp>

#include <memory>

using dish::reducer::PathChoice;
using dish::source::UsbPathChoiceMap;
using dish::source::UsbPathEntry;
using dish::source::usbPathKeyFor;
using dish::source::UsbPathPreferenceRepository;
using dish::source::UsbPathPreferenceStore;
using dish::test::makeSharedSettings;
using dish::test::StateSourceProbe;

namespace {

constexpr int kVidXbox = 0x045E, kPidXbox = 0x028E;
constexpr int kVidDs4 = 0x054C, kPidDs4 = 0x05C4;

} // namespace

TEST_CASE("UsbPathPreferenceRepository satisfies the contract", "[repository][usb-pathstore]") {
    auto settings = makeSharedSettings();
    dish::test::runRepositoryContract<QString, UsbPathEntry>(
        [&] { return std::make_unique<UsbPathPreferenceRepository>(makeSharedSettings()); },
        [](int i) { return QStringLiteral("%1:0001").arg(i, 4, 16, QLatin1Char('0')); },
        [](const QString& k) {
            return UsbPathEntry{k, k.startsWith(QLatin1Char('0')) ? PathChoice::Direct
                                                                  : PathChoice::Standard};
        });
}

TEST_CASE("setChoice then choiceFor round-trips per vid and pid", "[usb-pathstore]") {
    UsbPathPreferenceRepository repo(makeSharedSettings());
    UsbPathPreferenceStore store(&repo);

    store.setChoice(kVidXbox, kPidXbox, PathChoice::Direct);

    REQUIRE(store.choiceFor(kVidXbox, kPidXbox).has_value());
    CHECK(*store.choiceFor(kVidXbox, kPidXbox) == PathChoice::Direct);
    CHECK_FALSE(store.choiceFor(kVidDs4, kPidDs4).has_value());
}

TEST_CASE("choices survive into a fresh store over the same prefs", "[usb-pathstore]") {
    auto settings = makeSharedSettings();
    {
        UsbPathPreferenceRepository repo(settings);
        UsbPathPreferenceStore store(&repo);
        store.setChoice(kVidXbox, kPidXbox, PathChoice::Standard);
    }
    UsbPathPreferenceRepository reopenedRepo(settings);
    UsbPathPreferenceStore reopened(&reopenedRepo);

    REQUIRE(reopened.choiceFor(kVidXbox, kPidXbox).has_value());
    CHECK(*reopened.choiceFor(kVidXbox, kPidXbox) == PathChoice::Standard);
}

TEST_CASE("clear removes only the targeted pad", "[usb-pathstore]") {
    UsbPathPreferenceRepository repo(makeSharedSettings());
    UsbPathPreferenceStore store(&repo);
    store.setChoice(kVidXbox, kPidXbox, PathChoice::Direct);
    store.setChoice(kVidDs4, kPidDs4, PathChoice::Standard);

    store.clearChoice(kVidXbox, kPidXbox);

    CHECK_FALSE(store.choiceFor(kVidXbox, kPidXbox).has_value());
    REQUIRE(store.choiceFor(kVidDs4, kPidDs4).has_value());
    CHECK(*store.choiceFor(kVidDs4, kPidDs4) == PathChoice::Standard);
}

TEST_CASE("setChoice to the current value does not re-emit", "[usb-pathstore]") {
    UsbPathPreferenceRepository repo(makeSharedSettings());
    UsbPathPreferenceStore store(&repo);
    store.setChoice(kVidXbox, kPidXbox, PathChoice::Direct);

    // Probing after the first write, so count 1 is the eager initial emission.
    StateSourceProbe<UsbPathChoiceMap> probe(store.state());
    REQUIRE(probe.count() == 1);

    store.setChoice(kVidXbox, kPidXbox, PathChoice::Direct);

    CHECK(probe.count() == 1);
}

TEST_CASE("a corrupt choices blob falls back to an empty map without crashing", "[usb-pathstore]") {
    auto settings = makeSharedSettings();
    settings->setValue(QLatin1String(UsbPathPreferenceRepository::kChoicesKey),
                       QByteArray("{not valid json"));

    UsbPathPreferenceRepository repo(settings);
    UsbPathPreferenceStore store(&repo);

    CHECK_FALSE(store.choiceFor(kVidXbox, kPidXbox).has_value());
}

TEST_CASE("an unknown stored path value is dropped on read for forward-compat", "[usb-pathstore]") {
    auto settings = makeSharedSettings();
    // A PathChoice constant a newer build wrote. The key is "%04x:%04x" vid:pid.
    settings->setValue(QLatin1String(UsbPathPreferenceRepository::kChoicesKey),
                       QByteArray(R"({"045e:028e":"teleport"})"));

    UsbPathPreferenceRepository repo(settings);
    UsbPathPreferenceStore store(&repo);

    CHECK_FALSE(store.choiceFor(kVidXbox, kPidXbox).has_value());
    CHECK(usbPathKeyFor(kVidXbox, kPidXbox) == QStringLiteral("045e:028e"));
}
