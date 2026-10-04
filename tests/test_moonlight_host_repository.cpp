// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The remembered-Moonlight-host store: JSON round-trip, upsert-preserves-
// anchor semantics, and namespace isolation from the satellite family in the
// co-tenant settings file.

#include "repository/MoonlightHostRepository.h"

#include "QSettingsFixture.h"
#include "RepositoryContract.h"
#include "repository/SettingsKeys.h"

#include <catch2/catch_test_macros.hpp>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

using dish::repository::kMoonlightControllerTypeAuto;
using dish::repository::MoonlightBinding;
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

TEST_CASE("a controllerType of 0 migrates to the Auto sentinel", "[moonlight][repository]") {
    // 0 is CONTROLLER_TYPE_UNKNOWN on the wire, which asks the HOST to pick.
    // That is a different promise from "match the pad", so a record written
    // before the three clients converged on 0xFF is migrated on read rather
    // than sent as it stands.
    auto settings = makeSharedSettings();
    settings->setValue(
        QLatin1String(dish::repository::keys::kMoonlightHostListKey),
        QByteArray(R"({"legacy":{"uuid":"legacy","address":"10.0.0.9","controllerType":0}})"));

    MoonlightHostRepository repo(settings);
    const auto loaded = repo.get(QStringLiteral("legacy"));
    REQUIRE(loaded.has_value());
    CHECK(loaded->controllerType == kMoonlightControllerTypeAuto);
    CHECK(loaded->controllerType == 0xFF);
}

TEST_CASE("a controllerType outside the picker's range migrates too", "[moonlight][repository]") {
    auto settings = makeSharedSettings();
    settings->setValue(
        QLatin1String(dish::repository::keys::kMoonlightHostListKey),
        QByteArray(R"({"odd":{"uuid":"odd","address":"10.0.0.8","controllerType":42}})"));

    MoonlightHostRepository repo(settings);
    const auto loaded = repo.get(QStringLiteral("odd"));
    REQUIRE(loaded.has_value());
    CHECK(loaded->controllerType == kMoonlightControllerTypeAuto);
}

TEST_CASE("the three real picks survive a round trip untouched", "[moonlight][repository]") {
    auto settings = makeSharedSettings();
    MoonlightHostRepository repo(settings);
    for (const int pick : {1, 2, 3}) {
        MoonlightHost host;
        host.uuid = QStringLiteral("pick-%1").arg(pick);
        host.address = QStringLiteral("10.0.0.1");
        host.controllerType = pick;
        repo.upsert(host);
        const auto loaded = repo.get(host.uuid);
        REQUIRE(loaded.has_value());
        CHECK(loaded->controllerType == pick);
    }
}

// ── The standing bindings ────────────────────────────────────────────────────

namespace {

MoonlightBinding standing(const QString& slot, const QString& host, int type) {
    MoonlightBinding binding;
    binding.slotId = slot;
    binding.hostUuid = host;
    binding.controllerType = type;
    return binding;
}

} // namespace

TEST_CASE("a binding round-trips its host and its own controller type", "[moonlight][repository]") {
    auto settings = makeSharedSettings();
    MoonlightHostRepository repo(settings);
    const QString host = QStringLiteral("host-uuid");

    repo.rememberBinding(
        standing(QStringLiteral("sdl:1"), host, dish::moonproto::kControllerTypePs));
    // The type is PER BINDING: a second pad on the same host is a different device.
    repo.rememberBinding(
        standing(QStringLiteral("sdl:2"), host, dish::moonproto::kControllerTypeNintendo));

    REQUIRE(repo.bindings().size() == 2);
    CHECK(repo.binding(QStringLiteral("sdl:1"))->controllerType ==
          dish::moonproto::kControllerTypePs);
    CHECK(repo.binding(QStringLiteral("sdl:2"))->controllerType ==
          dish::moonproto::kControllerTypeNintendo);
    CHECK_FALSE(repo.binding(QStringLiteral("sdl:9")).has_value());

    repo.rememberBinding(
        standing(QStringLiteral("sdl:1"), host, dish::moonproto::kControllerTypeXbox));
    REQUIRE(repo.bindings().size() == 2);
    CHECK(repo.binding(QStringLiteral("sdl:1"))->controllerType ==
          dish::moonproto::kControllerTypeXbox);

    MoonlightHostRepository reopened(settings);
    REQUIRE(reopened.bindings().size() == 2);
    reopened.forgetBinding(QStringLiteral("sdl:1"));
    REQUIRE(reopened.bindings().size() == 1);
    CHECK_FALSE(reopened.binding(QStringLiteral("sdl:1")).has_value());

    // A record naming no slot or no host is not a binding.
    reopened.rememberBinding(standing(QString(), host, dish::moonproto::kControllerTypeAuto));
    reopened.rememberBinding(
        standing(QStringLiteral("sdl:3"), QString(), dish::moonproto::kControllerTypeAuto));
    REQUIRE(reopened.bindings().size() == 1);
}

TEST_CASE("a binding is stored under the key and fields dish-windows writes",
          "[moonlight][repository]") {
    auto settings = makeSharedSettings();
    MoonlightHostRepository repo(settings);
    repo.rememberBinding(standing(QStringLiteral("sdl:1"), QStringLiteral("host-uuid"),
                                  dish::moonproto::kControllerTypePs));

    const auto raw =
        settings->value(QLatin1String(dish::repository::keys::kMoonlightBindingListKey))
            .toByteArray();
    const auto doc = QJsonDocument::fromJson(raw);
    REQUIRE(doc.isArray());
    REQUIRE(doc.array().size() == 1);
    const auto obj = doc.array().first().toObject();
    CHECK(obj.value(QLatin1String("slotId")).toString() == QStringLiteral("sdl:1"));
    CHECK(obj.value(QLatin1String("hostId")).toString() == QStringLiteral("host-uuid"));
    CHECK(obj.value(QLatin1String("controllerType")).toInt() == dish::moonproto::kControllerTypePs);
}

TEST_CASE("a binding stored with the old Auto is migrated too", "[moonlight][repository]") {
    QJsonObject legacy;
    legacy[QStringLiteral("slotId")] = QStringLiteral("sdl:1");
    legacy[QStringLiteral("hostId")] = QStringLiteral("host-uuid");
    legacy[QStringLiteral("controllerType")] = 0;
    const auto migrated = MoonlightBinding::fromJson(legacy);
    REQUIRE(migrated.has_value());
    CHECK(migrated->controllerType == kMoonlightControllerTypeAuto);
    CHECK_FALSE(MoonlightBinding::fromJson(QJsonObject()).has_value());
}

TEST_CASE("forgetting a host retires the bindings that drove it", "[moonlight][repository]") {
    auto settings = makeSharedSettings();
    MoonlightHostRepository repo(settings);
    const QString gone = QStringLiteral("host-gone");
    const QString kept = QStringLiteral("host-kept");
    repo.rememberBinding(
        standing(QStringLiteral("sdl:1"), gone, dish::moonproto::kControllerTypeAuto));
    repo.rememberBinding(
        standing(QStringLiteral("sdl:2"), gone, dish::moonproto::kControllerTypeAuto));
    repo.rememberBinding(
        standing(QStringLiteral("sdl:3"), kept, dish::moonproto::kControllerTypeAuto));
    REQUIRE(repo.bindings().size() == 3);

    repo.forgetBindingsForHost(gone);

    REQUIRE(repo.bindings().size() == 1);
    CHECK(repo.bindings().front().slotId == QStringLiteral("sdl:3"));
}

TEST_CASE("a binding keeps the pad's identity, and a record from before it existed reads as none",
          "[moonlight][repository]") {
    auto settings = makeSharedSettings();
    MoonlightHostRepository repo(settings);
    MoonlightBinding withPad;
    withPad.slotId = QStringLiteral("sdl:1");
    withPad.hostUuid = QStringLiteral("host-uuid");
    withPad.padIdentity = QStringLiteral("guid:0300aabb/serial:11:22:33");
    repo.rememberBinding(withPad);

    const auto back = MoonlightHostRepository(settings).binding(QStringLiteral("sdl:1"));
    REQUIRE(back.has_value());
    CHECK(*back == withPad);
    CHECK(back->padIdentity == QStringLiteral("guid:0300aabb/serial:11:22:33"));

    const auto raw =
        settings->value(QLatin1String(dish::repository::keys::kMoonlightBindingListKey))
            .toByteArray();
    const auto obj = QJsonDocument::fromJson(raw).array().first().toObject();
    CHECK(obj.value(QLatin1String("padIdentity")).toString() ==
          QStringLiteral("guid:0300aabb/serial:11:22:33"));

    QJsonObject old;
    old.insert(QStringLiteral("slotId"), QStringLiteral("sdl:2"));
    old.insert(QStringLiteral("hostId"), QStringLiteral("host-uuid"));
    const auto legacy = MoonlightBinding::fromJson(old);
    REQUIRE(legacy.has_value());
    CHECK(legacy->padIdentity.isEmpty());
}
