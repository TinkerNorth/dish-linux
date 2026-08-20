// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "repository/DeadzoneRepository.h"
#include "repository/SatellitePinRepository.h"

#include "QSettingsFixture.h"
#include "RepositoryContract.h"

#include <catch2/catch_test_macros.hpp>

#include <QString>

using dish::repository::DeadzoneEntry;
using dish::repository::DeadzoneRepository;
using dish::repository::SatellitePinRepository;
using dish::test::makeSharedSettings;

TEST_CASE("DeadzoneRepository satisfies the repository contract", "[repository][deadzone]") {
    dish::test::runRepositoryContract<QString, DeadzoneEntry>(
        [] { return std::make_unique<DeadzoneRepository>(makeSharedSettings()); },
        [](int i) { return QStringLiteral("dev-%1").arg(i); },
        [](const QString& k) {
            return DeadzoneEntry{k, static_cast<std::int16_t>(1000 + k.size()),
                                 static_cast<std::uint8_t>(5)};
        });
}

TEST_CASE("setDeadzones then read round-trips by device", "[deadzone]") {
    DeadzoneRepository repo(makeSharedSettings());
    repo.setDeadzones("pad-1", {5000, 20});
    const auto dz = repo.deadzonesFor("pad-1");
    REQUIRE(dz.has_value());
    CHECK(dz->stickFlat == 5000);
    CHECK(dz->triggerFlat == 20);
}

TEST_CASE("an unset device has no deadzone override", "[deadzone]") {
    DeadzoneRepository repo(makeSharedSettings());
    CHECK_FALSE(repo.deadzonesFor("never-set").has_value());
}

TEST_CASE("deadzones survive into a fresh repo over the same store", "[deadzone]") {
    auto store = makeSharedSettings();
    DeadzoneRepository(store).setDeadzones("pad-1", {3277, 13});
    DeadzoneRepository reopened(store);
    const auto dz = reopened.deadzonesFor("pad-1");
    REQUIRE(dz.has_value());
    CHECK(dz->stickFlat == 3277);
    CHECK(dz->triggerFlat == 13);
}

TEST_CASE("distinct devices keep isolated deadzone profiles", "[deadzone]") {
    DeadzoneRepository repo(makeSharedSettings());
    repo.setDeadzones("a", {1000, 1});
    repo.setDeadzones("b", {9000, 99});
    CHECK(repo.deadzonesFor("a")->stickFlat == 1000);
    CHECK(repo.deadzonesFor("b")->stickFlat == 9000);
    CHECK(repo.deadzonesFor("b")->triggerFlat == 99);
}

TEST_CASE("remove drops one device profile and leaves the others", "[deadzone]") {
    DeadzoneRepository repo(makeSharedSettings());
    repo.setDeadzones("a", {1000, 1});
    repo.setDeadzones("b", {2000, 2});
    repo.remove("a");
    CHECK_FALSE(repo.deadzonesFor("a").has_value());
    CHECK(repo.deadzonesFor("b").has_value());
}

TEST_CASE("clear wipes only the deadzone namespace", "[deadzone]") {
    auto store = makeSharedSettings();
    DeadzoneRepository deadzones(store);
    SatellitePinRepository pins(store);
    deadzones.setDeadzones("pad-1", {4000, 10});
    pins.pin("satellite:mid:x", "CAFE");

    deadzones.clear();

    CHECK_FALSE(deadzones.deadzonesFor("pad-1").has_value());
    CHECK(pins.pinnedFingerprint("satellite:mid:x") == QStringLiteral("CAFE"));
}

TEST_CASE("a deadzone does not leak into the sibling pin namespace", "[deadzone]") {
    auto store = makeSharedSettings();
    DeadzoneRepository deadzones(store);
    SatellitePinRepository pins(store);
    deadzones.setDeadzones("pad-1", {4000, 10});
    CHECK_FALSE(pins.pinnedFingerprint("pad-1").has_value());
}

TEST_CASE("a corrupt stored value reads back as absent, not a crash", "[deadzone]") {
    auto store = makeSharedSettings();
    store->setValue(QStringLiteral("deadzone:pad-1"), QByteArrayLiteral("{not json"));
    DeadzoneRepository repo(store);
    CHECK_FALSE(repo.deadzonesFor("pad-1").has_value());
    CHECK(repo.all().empty());
}

TEST_CASE("keyOf returns the entry's device id", "[deadzone]") {
    DeadzoneRepository repo(makeSharedSettings());
    const DeadzoneEntry e{"pad-7", 1234, 7};
    CHECK(repo.keyOf(e) == QStringLiteral("pad-7"));
    // The value overload of put() routes through keyOf.
    repo.put(e);
    CHECK(repo.deadzonesFor("pad-7")->stickFlat == 1234);
}
