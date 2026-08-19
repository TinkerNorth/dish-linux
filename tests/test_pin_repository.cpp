// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "repository/SatellitePinRepository.h"
#include "repository/SatelliteSharedKeyRepository.h"

#include "QSettingsFixture.h"
#include "RepositoryContract.h"

#include <catch2/catch_test_macros.hpp>

#include <QString>

using dish::repository::SatellitePinRepository;
using dish::repository::SatelliteSharedKeyRepository;
using dish::test::makeSharedSettings;

TEST_CASE("SatellitePinRepository satisfies the repository contract", "[repository][pin]") {
    dish::test::runRepositoryContract<QString, QString>(
        [] { return std::make_unique<SatellitePinRepository>(makeSharedSettings()); },
        [](int i) { return QStringLiteral("satellite:mid:%1").arg(i); },
        [](const QString& k) { return QStringLiteral("fp-") + k; });
}

TEST_CASE("pin then read round-trips by id", "[pin]") {
    SatellitePinRepository pins(makeSharedSettings());
    pins.pin("satellite:mid:abc", "deadbeef");
    CHECK(pins.pinnedFingerprint("satellite:mid:abc") == QStringLiteral("deadbeef"));
}

TEST_CASE("an unknown satellite has no pin", "[pin]") {
    SatellitePinRepository pins(makeSharedSettings());
    CHECK_FALSE(pins.pinnedFingerprint("satellite:mid:nope").has_value());
}

TEST_CASE("pins survive into a fresh repo over the same store", "[pin]") {
    auto store = makeSharedSettings();
    SatellitePinRepository(store).pin("satellite:mid:abc", "AABB");
    SatellitePinRepository reopened(store);
    CHECK(reopened.pinnedFingerprint("satellite:mid:abc") == QStringLiteral("AABB"));
}

TEST_CASE("forget drops one pin and leaves the others", "[pin]") {
    SatellitePinRepository pins(makeSharedSettings());
    pins.pin("satellite:mid:a", "AA");
    pins.pin("satellite:mid:b", "BB");
    pins.forget("satellite:mid:a");
    CHECK_FALSE(pins.pinnedFingerprint("satellite:mid:a").has_value());
    CHECK(pins.pinnedFingerprint("satellite:mid:b") == QStringLiteral("BB"));
}

TEST_CASE("distinct satellites keep isolated pins", "[pin]") {
    SatellitePinRepository pins(makeSharedSettings());
    pins.pin("10.0.0.1", "1111");
    pins.pin("10.0.0.2", "2222");
    CHECK(pins.pinnedFingerprint("10.0.0.1") == QStringLiteral("1111"));
    CHECK(pins.pinnedFingerprint("10.0.0.2") == QStringLiteral("2222"));
}

TEST_CASE("a pin does not leak into the sibling shared-key namespace", "[pin]") {
    auto store = makeSharedSettings();
    SatellitePinRepository pins(store);
    SatelliteSharedKeyRepository keys(store);
    pins.pin("satellite:mid:a", "CAFE");
    CHECK_FALSE(keys.get("satellite:mid:a").has_value());
    CHECK(SatellitePinRepository(store).pinnedFingerprint("satellite:mid:a") ==
          QStringLiteral("CAFE"));
}
