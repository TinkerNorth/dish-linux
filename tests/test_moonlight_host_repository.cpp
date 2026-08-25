// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The remembered-Moonlight-host store: JSON round-trip, upsert-preserves-
// anchor semantics, and namespace isolation from the satellite family in the
// co-tenant settings file.

#include "repository/MoonlightHostRepository.h"

#include "QSettingsFixture.h"
#include "RepositoryContract.h"

#include <catch2/catch_test_macros.hpp>

using dish::repository::kMoonlightControllerTypeAuto;
using dish::repository::MoonlightHost;
using dish::repository::MoonlightHostRepository;
using dish::test::makeSharedSettings;

namespace {

MoonlightHost sampleHost(const QString& uuid) {
    MoonlightHost host;
    host.uuid = uuid;
    host.name = QStringLiteral("Living Room PC");
    host.address = QStringLiteral("192.168.1.42");
    host.serverCertPem =
        QStringLiteral("-----BEGIN CERTIFICATE-----\nABC\n-----END CERTIFICATE-----\n");
    host.lastAppId = QStringLiteral("881448767");
    host.lastAppName = QStringLiteral("Desktop");
    host.controllerType = 2;
    return host;
}

} // namespace

TEST_CASE("MoonlightHostRepository satisfies the repository contract", "[repository][moonlight]") {
    dish::test::runRepositoryContract<QString, MoonlightHost>(
        [] { return std::make_unique<MoonlightHostRepository>(makeSharedSettings()); },
        [](int i) { return QStringLiteral("uuid-%1").arg(i); },
        [](const QString& k) {
            MoonlightHost h;
            h.uuid = k;
            h.name = QStringLiteral("host-") + k;
            h.address = QStringLiteral("10.0.0.") + k.right(1);
            return h;
        });
}

TEST_CASE("a host round-trips through JSON", "[moonlight][repository]") {
    MoonlightHostRepository repo(makeSharedSettings());
    const auto host = sampleHost(QStringLiteral("uuid-a"));
    repo.upsert(host);
    const auto loaded = repo.get(QStringLiteral("uuid-a"));
    REQUIRE(loaded.has_value());
    CHECK(*loaded == host);
    CHECK(loaded->paired());
}

TEST_CASE("upsert preserves the pairing anchor and app pick on re-discovery",
          "[moonlight][repository]") {
    auto store = makeSharedSettings();
    MoonlightHostRepository repo(store);
    repo.upsert(sampleHost(QStringLiteral("uuid-a")));

    // A bare re-discovery: same uuid, new address, no cert or app.
    MoonlightHost rediscovered;
    rediscovered.uuid = QStringLiteral("uuid-a");
    rediscovered.address = QStringLiteral("192.168.1.99");
    repo.upsert(rediscovered);

    const auto loaded = repo.get(QStringLiteral("uuid-a"));
    REQUIRE(loaded.has_value());
    CHECK(loaded->address == QStringLiteral("192.168.1.99")); // address updated
    CHECK(loaded->paired());                                  // cert preserved
    CHECK(loaded->lastAppId == QStringLiteral("881448767"));  // pick preserved
    CHECK(loaded->name == QStringLiteral("Living Room PC"));  // name preserved
}

TEST_CASE("an empty uuid is not stored", "[moonlight][repository]") {
    MoonlightHostRepository repo(makeSharedSettings());
    MoonlightHost host;
    host.address = QStringLiteral("1.2.3.4");
    repo.upsert(host);
    CHECK(repo.all().empty());
}

TEST_CASE("controllerType defaults to Auto when absent", "[moonlight][repository]") {
    MoonlightHostRepository repo(makeSharedSettings());
    MoonlightHost host;
    host.uuid = QStringLiteral("uuid-x");
    host.address = QStringLiteral("1.2.3.4");
    repo.put(QStringLiteral("uuid-x"), host);
    const auto loaded = repo.get(QStringLiteral("uuid-x"));
    REQUIRE(loaded.has_value());
    CHECK(loaded->controllerType == kMoonlightControllerTypeAuto);
}

TEST_CASE("fromJson rejects rows without a uuid or address", "[moonlight][repository]") {
    QJsonObject noUuid;
    noUuid.insert(QStringLiteral("address"), QStringLiteral("1.2.3.4"));
    CHECK_FALSE(MoonlightHost::fromJson(noUuid).has_value());

    QJsonObject noAddr;
    noAddr.insert(QStringLiteral("uuid"), QStringLiteral("u"));
    CHECK_FALSE(MoonlightHost::fromJson(noAddr).has_value());
}
