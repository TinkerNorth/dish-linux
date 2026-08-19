// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "repository/SatelliteSharedKeyRepository.h"

#include "QSettingsFixture.h"
#include "RepositoryContract.h"
#include "repository/SettingsKeys.h"

#include <catch2/catch_test_macros.hpp>

#include <QString>

#include <algorithm>

using dish::repository::SatelliteSharedKeyRepository;
using dish::test::makeSharedSettings;
namespace keys = dish::repository::keys;

namespace {
bool contains(const std::vector<QString>& v, const QString& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}
} // namespace

TEST_CASE("SatelliteSharedKeyRepository satisfies the repository contract", "[repository][key]") {
    dish::test::runRepositoryContract<QString, QString>(
        [] { return std::make_unique<SatelliteSharedKeyRepository>(makeSharedSettings()); },
        [](int i) { return QStringLiteral("satellite:mid:%1").arg(i); },
        [](const QString& k) { return QStringLiteral("key-") + k; });
}

TEST_CASE("put then get round-trips by id", "[key]") {
    SatelliteSharedKeyRepository keysRepo(makeSharedSettings());
    keysRepo.put("satellite:mid:abc", "DEADBEEF");
    CHECK(keysRepo.get("satellite:mid:abc") == QStringLiteral("DEADBEEF"));
}

TEST_CASE("an unknown id has no key", "[key]") {
    SatelliteSharedKeyRepository keysRepo(makeSharedSettings());
    CHECK_FALSE(keysRepo.get("satellite:mid:nope").has_value());
}

TEST_CASE("keys survive into a fresh repo over the same store", "[key]") {
    auto store = makeSharedSettings();
    SatelliteSharedKeyRepository(store).put("satellite:mid:abc", "KEY1");
    CHECK(SatelliteSharedKeyRepository(store).get("satellite:mid:abc") == QStringLiteral("KEY1"));
}

TEST_CASE("remove drops one key and leaves the others", "[key]") {
    SatelliteSharedKeyRepository keysRepo(makeSharedSettings());
    keysRepo.put("satellite:mid:a", "A");
    keysRepo.put("satellite:mid:b", "B");
    keysRepo.remove("satellite:mid:a");
    CHECK_FALSE(keysRepo.get("satellite:mid:a").has_value());
    CHECK(keysRepo.get("satellite:mid:b") == QStringLiteral("B"));
}

TEST_CASE("all returns only shared-key values, ignoring co-tenant entries", "[key]") {
    auto store = makeSharedSettings();
    // Co-tenant entries that must NOT appear in all().
    store->setValue(QLatin1String(keys::kSatelliteListKey), "[]");
    store->setValue(QLatin1String(keys::kCertPinPrefix) + QStringLiteral("x"), "pin-value");
    SatelliteSharedKeyRepository keysRepo(store);
    keysRepo.put("satellite:mid:a", "A");
    keysRepo.put("satellite:mid:b", "B");
    const auto all = keysRepo.all();
    CHECK(all.size() == 2);
    CHECK(contains(all, QStringLiteral("A")));
    CHECK(contains(all, QStringLiteral("B")));
    CHECK_FALSE(contains(all, QStringLiteral("pin-value")));
}

TEST_CASE("clear removes every shared key but preserves co-tenant prefs", "[key]") {
    auto store = makeSharedSettings();
    store->setValue(QLatin1String(keys::kSatelliteListKey), "preserved");
    SatelliteSharedKeyRepository keysRepo(store);
    keysRepo.put("satellite:mid:a", "A");
    keysRepo.put("satellite:mid:b", "B");
    keysRepo.clear();
    CHECK(keysRepo.all().empty());
    CHECK(store->value(QLatin1String(keys::kSatelliteListKey)).toString() ==
          QStringLiteral("preserved"));
}
