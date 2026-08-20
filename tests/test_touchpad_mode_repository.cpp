// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The resolve ladder owns the touchpad-mode default, so this layer reads back
// nullopt for an unwritten satellite rather than "off".

#include "repository/TouchpadModeRepository.h"

#include "QSettingsFixture.h"
#include "RepositoryContract.h"

#include <catch2/catch_test_macros.hpp>

#include <QString>

using dish::repository::TouchpadModePreference;
using dish::repository::TouchpadModeRepository;
using dish::test::makeSharedSettings;

TEST_CASE("TouchpadModeRepository satisfies the repository contract",
          "[repository][touchpad-mode]") {
    dish::test::runRepositoryContract<QString, TouchpadModePreference>(
        [] { return std::make_unique<TouchpadModeRepository>(makeSharedSettings()); },
        [](int i) { return QStringLiteral("sat-%1").arg(i); },
        [](const QString& k) {
            return TouchpadModePreference{k, (k.size() % 2) == 0 ? QStringLiteral("ds4")
                                                                 : QStringLiteral("off")};
        });
}

TEST_CASE("touchpad-mode picks round-trip with durability", "[touchpad-mode-repo]") {
    auto store = makeSharedSettings();
    {
        TouchpadModeRepository repo(store);
        repo.put(TouchpadModePreference{"sat-a", "ds4"});
        repo.put(TouchpadModePreference{"sat-b", "off"});
    }
    // A fresh repo over the same store: no in-memory cache masks a broken persist.
    TouchpadModeRepository repo2(store);
    REQUIRE(repo2.get("sat-a").has_value());
    CHECK(repo2.get("sat-a")->mode == "ds4");
    REQUIRE(repo2.get("sat-b").has_value());
    CHECK(repo2.get("sat-b")->mode == "off");
}

TEST_CASE("get on a never-picked satellite returns nullopt, not a default",
          "[touchpad-mode-repo]") {
    TouchpadModeRepository repo(makeSharedSettings());
    CHECK_FALSE(repo.get("never-picked").has_value());
}

TEST_CASE("corrupt payload falls back to empty, does not crash", "[touchpad-mode-repo]") {
    auto store = makeSharedSettings();
    store->setValue(QLatin1String(dish::repository::kTouchpadModeListKey),
                    QByteArrayLiteral("{not valid json"));
    TouchpadModeRepository repo(store);
    CHECK(repo.all().empty());
    CHECK_FALSE(repo.get("anything").has_value());
}

TEST_CASE("remove of one satellite does not disturb the others", "[touchpad-mode-repo]") {
    TouchpadModeRepository repo(makeSharedSettings());
    repo.put(TouchpadModePreference{"keep", "ds4"});
    repo.put(TouchpadModePreference{"drop", "mouse"});
    repo.remove(QStringLiteral("drop"));
    CHECK_FALSE(repo.get("drop").has_value());
    REQUIRE(repo.get("keep").has_value());
    CHECK(repo.get("keep")->mode == "ds4");
}

TEST_CASE("put on the same satellite replaces in place", "[touchpad-mode-repo]") {
    TouchpadModeRepository repo(makeSharedSettings());
    repo.put(TouchpadModePreference{"sat", "off"});
    repo.put(TouchpadModePreference{"sat", "ds4"});
    REQUIRE(repo.all().size() == 1);
    CHECK(repo.get("sat")->mode == "ds4");
}
