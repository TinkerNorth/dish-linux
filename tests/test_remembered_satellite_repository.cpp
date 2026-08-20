// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "repository/RememberedSatelliteRepository.h"

#include "QSettingsFixture.h"
#include "RepositoryContract.h"
#include "repository/SettingsKeys.h"

#include <catch2/catch_test_macros.hpp>

#include <QString>

using dish::models::RememberedWifi;
using dish::repository::RememberedSatelliteRepository;
using dish::test::makeSharedSettings;
namespace keys = dish::repository::keys;

namespace {
RememberedWifi sat(const QString& id, const QString& name = QStringLiteral("Box"),
                   const QString& ip = QStringLiteral("10.0.0.1")) {
    RememberedWifi r;
    r.id = id;
    r.name = name;
    r.ip = ip;
    r.udpPort = 9876;
    r.pairPort = 9443;
    r.httpPort = 9443;
    return r;
}
} // namespace

TEST_CASE("RememberedSatelliteRepository satisfies the repository contract", "[repository][rem]") {
    dish::test::runRepositoryContract<QString, RememberedWifi>(
        [] { return std::make_unique<RememberedSatelliteRepository>(makeSharedSettings()); },
        [](int i) { return QStringLiteral("satellite:mid:%1").arg(i); },
        [](const QString& k) { return sat(k, QStringLiteral("name-") + k); });
}

TEST_CASE("put then get round-trips by id", "[rem]") {
    RememberedSatelliteRepository repo(makeSharedSettings());
    repo.put(sat("satellite:mid:abc", "Den"));
    const auto got = repo.get("satellite:mid:abc");
    REQUIRE(got.has_value());
    CHECK(got->name == QStringLiteral("Den"));
}

TEST_CASE("entries survive into a fresh repo over the same store", "[rem]") {
    auto store = makeSharedSettings();
    RememberedSatelliteRepository(store).put(sat("satellite:mid:abc"));
    const auto got = RememberedSatelliteRepository(store).get("satellite:mid:abc");
    REQUIRE(got.has_value());
    CHECK(got->name == QStringLiteral("Box"));
}

TEST_CASE("corrupt JSON falls back to empty without crashing", "[rem]") {
    auto store = makeSharedSettings();
    store->setValue(QLatin1String(keys::kSatelliteListKey), "{not valid json");
    RememberedSatelliteRepository repo(store);
    CHECK(repo.all().empty());
    CHECK_FALSE(repo.get("anything").has_value());
}

TEST_CASE("a non-array JSON blob also falls back to empty", "[rem]") {
    auto store = makeSharedSettings();
    store->setValue(QLatin1String(keys::kSatelliteListKey), "{\"id\":\"x\"}");
    RememberedSatelliteRepository repo(store);
    CHECK(repo.all().empty());
}
