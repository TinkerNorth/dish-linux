// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The session is PER HOST and reference counted, and this is where that is
// enforced. A Moonlight session carries up to four controllers behind one
// launch, so a second binding on a host must join what is already there rather
// than start a second session beside it, and only the LAST unbind may tear it
// down and hand the app back.
//
// Nothing here reaches the network: every assertion is about the bookkeeping
// the coordinator does before a socket is involved, which is exactly the part
// that would otherwise only be observable against a live host.

#include "QSettingsFixture.h"
#include "core/moonlight/MoonlightPadSlots.h"
#include "core/moonlight/MoonlightSessionMachine.h"
#include "repository/MoonlightHostRepository.h"
#include "source/moonlight/MoonlightManager.h"

#include <catch2/catch_test_macros.hpp>

#include <QStringList>

using namespace dish;
using namespace dish::source::moon;

namespace {

// QSignalSpy stand-in: DishTests links Catch2, not Qt6::Test.
struct AppsSpy {
    QStringList hosts;

    explicit AppsSpy(MoonlightManager* manager) {
        QObject::connect(manager, &MoonlightManager::appsChanged,
                         [this](const QString& uuid) { hosts.append(uuid); });
    }
};

// A host the manager treats as paired. The certificate is never presented here
// (nothing dials), only the "we remember pairing this" flag it stands for.
repository::MoonlightHost pairedHost(const QString& uuid = QStringLiteral("host-uuid")) {
    repository::MoonlightHost host;
    host.uuid = uuid;
    host.name = QStringLiteral("Living room PC");
    // An address that resolves nowhere, so the probe this triggers can never
    // reach a real machine on the developer's network.
    host.address = QStringLiteral("192.0.2.1");
    host.serverCertPem = QStringLiteral("-----BEGIN CERTIFICATE-----\nnot-a-real-cert\n"
                                        "-----END CERTIFICATE-----\n");
    return host;
}

moonlight::SourceCapabilities plainPad() {
    moonlight::SourceCapabilities source;
    source.rumble = true;
    return source;
}

moonlight::SourceCapabilities motionPad() {
    moonlight::SourceCapabilities source;
    source.rumble = true;
    source.motion = true;
    return source;
}

} // namespace

TEST_CASE("a second binding joins the session instead of starting another",
          "[moonlight][binding]") {
    auto settings = test::makeSharedSettings();
    repository::MoonlightHostRepository repo(settings);
    repo.upsert(pairedHost());

    MoonlightManager manager(settings);
    const QString uuid = QStringLiteral("host-uuid");

    const auto first = manager.bindController(QStringLiteral("pad-a"), uuid,
                                              moonproto::kControllerTypeAuto, motionPad());
    REQUIRE(first.has_value());
    CHECK(*first == 0);

    auto* session = manager.session(uuid);
    REQUIRE(session != nullptr);
    CHECK(session->everStarted());
    // The launch is under way, so the session is no longer resting.
    CHECK_FALSE(moonlight::sessionNeedsStart(session->machineState().phase));

    const auto second = manager.bindController(QStringLiteral("pad-b"), uuid,
                                               moonproto::kControllerTypeXbox, plainPad());
    REQUIRE(second.has_value());
    CHECK(*second == 1);
    // ONE session object, still the same one: a second would mean a second
    // /launch and a host refusing it as "an app is already running".
    CHECK(manager.session(uuid) == session);
    CHECK(session->controllerCount() == 2);
    CHECK(manager.controllerCount(uuid) == 2);
}

TEST_CASE("four controllers ride one host and the fifth is refused", "[moonlight][binding]") {
    auto settings = test::makeSharedSettings();
    repository::MoonlightHostRepository repo(settings);
    repo.upsert(pairedHost());

    MoonlightManager manager(settings);
    const QString uuid = QStringLiteral("host-uuid");

    for (int i = 0; i < 4; ++i) {
        const auto number = manager.bindController(QStringLiteral("pad-%1").arg(i), uuid,
                                                   moonproto::kControllerTypeAuto, plainPad());
        REQUIRE(number.has_value());
        CHECK(static_cast<int>(*number) == i);
    }
    CHECK(manager.controllerCount(uuid) == static_cast<int>(moonlight::kMaxPads));

    // The hard protocol limit, and the only host state that refuses a binding.
    CHECK_FALSE(manager
                    .bindController(QStringLiteral("pad-4"), uuid, moonproto::kControllerTypeAuto,
                                    plainPad())
                    .has_value());
    CHECK(manager.boundHostFor(QStringLiteral("pad-4")).isEmpty());
    CHECK(manager.controllerCount(uuid) == static_cast<int>(moonlight::kMaxPads));

    // And the render contract agrees with the refusal.
    const auto inputs = manager.uiInputs(uuid, QStringLiteral("pad-4"));
    CHECK(inputs.otherControllers == static_cast<int>(moonlight::kMaxPads));
    CHECK(moonlight::sessionUiState(inputs) == moonlight::SessionUiState::HostFull);
    CHECK(moonlight::sessionUiBlocksApply(moonlight::sessionUiState(inputs)));
}

TEST_CASE("only the last unbind tears the session down", "[moonlight][binding]") {
    auto settings = test::makeSharedSettings();
    repository::MoonlightHostRepository repo(settings);
    repo.upsert(pairedHost());

    MoonlightManager manager(settings);
    const QString uuid = QStringLiteral("host-uuid");

    for (int i = 0; i < 4; ++i) {
        REQUIRE(manager
                    .bindController(QStringLiteral("pad-%1").arg(i), uuid,
                                    moonproto::kControllerTypeAuto, plainPad())
                    .has_value());
    }
    auto* session = manager.session(uuid);
    REQUIRE(session != nullptr);

    for (int i = 0; i < 3; ++i) {
        manager.unbindController(QStringLiteral("pad-%1").arg(i));
        CHECK(manager.controllerCount(uuid) == 3 - i);
        // Still carrying somebody, so the launch is left exactly as it was.
        CHECK_FALSE(moonlight::sessionNeedsStart(session->machineState().phase));
    }

    manager.unbindController(QStringLiteral("pad-3"));
    CHECK(manager.controllerCount(uuid) == 0);
    // Nobody is riding it, so the session is stopped and the app handed back.
    CHECK(session->machineState().phase == moonlight::SessionPhase::Idle);
    CHECK(manager.boundHostFor(QStringLiteral("pad-3")).isEmpty());
}

TEST_CASE("a freed controller number is handed to the next binding", "[moonlight][binding]") {
    auto settings = test::makeSharedSettings();
    repository::MoonlightHostRepository repo(settings);
    repo.upsert(pairedHost());

    MoonlightManager manager(settings);
    const QString uuid = QStringLiteral("host-uuid");

    REQUIRE(manager.bindController(QStringLiteral("a"), uuid, moonproto::kControllerTypeAuto,
                                   plainPad()) == 0);
    REQUIRE(manager.bindController(QStringLiteral("b"), uuid, moonproto::kControllerTypeAuto,
                                   plainPad()) == 1);
    REQUIRE(manager.bindController(QStringLiteral("c"), uuid, moonproto::kControllerTypeAuto,
                                   plainPad()) == 2);

    manager.unbindController(QStringLiteral("b"));
    // The lowest FREE index, which is the one just released.
    CHECK(manager.bindController(QStringLiteral("d"), uuid, moonproto::kControllerTypeAuto,
                                 plainPad()) == 1);
    CHECK(manager.controllerNumber(QStringLiteral("a")) == 0);
    CHECK(manager.controllerNumber(QStringLiteral("c")) == 2);
    CHECK(manager.controllerNumber(QStringLiteral("d")) == 1);
}

TEST_CASE("re-binding a slot that already holds a number is a restart", "[moonlight][binding]") {
    // What Reconnect after a drop does. A second attach for the same slot would
    // be skipped by the host anyway, and losing the binding would be worse.
    auto settings = test::makeSharedSettings();
    repository::MoonlightHostRepository repo(settings);
    repo.upsert(pairedHost());

    MoonlightManager manager(settings);
    const QString uuid = QStringLiteral("host-uuid");
    REQUIRE(manager.bindController(QStringLiteral("a"), uuid, moonproto::kControllerTypeAuto,
                                   plainPad()) == 0);
    CHECK(manager.bindController(QStringLiteral("a"), uuid, moonproto::kControllerTypeAuto,
                                 plainPad()) == 0);
    CHECK(manager.controllerCount(uuid) == 1);
}

TEST_CASE("a binding to an unpaired host is still recorded", "[moonlight][binding]") {
    // A binding is a DURABLE INTENT: pairing is remembered trust verified
    // lazily, so the session is attempted when the controller is used and never
    // when the binding is saved. Nothing about the host may refuse the answer.
    auto settings = test::makeSharedSettings();
    repository::MoonlightHost unpaired = pairedHost(QStringLiteral("cold-host"));
    unpaired.serverCertPem.clear();
    repository::MoonlightHostRepository repo(settings);
    repo.upsert(unpaired);

    MoonlightManager manager(settings);
    const QString uuid = QStringLiteral("cold-host");

    CHECK_FALSE(
        manager
            .bindController(QStringLiteral("pad"), uuid, moonproto::kControllerTypeAuto, plainPad())
            .has_value());
    // No controller number, because there is no session to hold one. The
    // binding stands regardless.
    CHECK(manager.boundHostFor(QStringLiteral("pad")) == uuid);
    CHECK(manager.session(uuid) == nullptr);

    const auto inputs = manager.uiInputs(uuid, QStringLiteral("pad"));
    CHECK_FALSE(moonlight::sessionUiBlocksApply(moonlight::sessionUiState(inputs)));
}

TEST_CASE("a slot drives exactly one destination", "[moonlight][binding]") {
    auto settings = test::makeSharedSettings();
    repository::MoonlightHostRepository repo(settings);
    repo.upsert(pairedHost(QStringLiteral("first-host")));
    repo.upsert(pairedHost(QStringLiteral("second-host")));

    MoonlightManager manager(settings);
    REQUIRE(manager
                .bindController(QStringLiteral("pad"), QStringLiteral("first-host"),
                                moonproto::kControllerTypeAuto, plainPad())
                .has_value());
    REQUIRE(manager
                .bindController(QStringLiteral("pad"), QStringLiteral("second-host"),
                                moonproto::kControllerTypeAuto, plainPad())
                .has_value());

    CHECK(manager.boundHostFor(QStringLiteral("pad")) == QStringLiteral("second-host"));
    CHECK(manager.controllerCount(QStringLiteral("first-host")) == 0);
    CHECK(manager.controllerCount(QStringLiteral("second-host")) == 1);
    // Moving away emptied the first host's session, so it was torn down.
    auto* first = manager.session(QStringLiteral("first-host"));
    REQUIRE(first != nullptr);
    CHECK(first->machineState().phase == moonlight::SessionPhase::Idle);
}

TEST_CASE("forgetting a host drops the bindings that rode it", "[moonlight][binding]") {
    auto settings = test::makeSharedSettings();
    repository::MoonlightHostRepository repo(settings);
    repo.upsert(pairedHost());

    MoonlightManager manager(settings);
    const QString uuid = QStringLiteral("host-uuid");
    REQUIRE(
        manager
            .bindController(QStringLiteral("a"), uuid, moonproto::kControllerTypeAuto, plainPad())
            .has_value());
    REQUIRE(
        manager
            .bindController(QStringLiteral("b"), uuid, moonproto::kControllerTypeAuto, plainPad())
            .has_value());

    manager.forget(uuid);
    CHECK(manager.boundHostFor(QStringLiteral("a")).isEmpty());
    CHECK(manager.boundHostFor(QStringLiteral("b")).isEmpty());
    CHECK_FALSE(manager.knows(uuid));
}

TEST_CASE("the app list is per host and a refusal is not an empty list", "[moonlight][binding]") {
    // /applist is HTTPS and paired-only, so an unpaired host answers 404.
    // Reading that as "no apps" would present a refusal as a fact about the
    // host, and the copy for the two states says different things.
    auto settings = test::makeSharedSettings();
    repository::MoonlightHost unpaired = pairedHost(QStringLiteral("cold-host"));
    unpaired.serverCertPem.clear();
    repository::MoonlightHostRepository repo(settings);
    repo.upsert(unpaired);

    MoonlightManager manager(settings);
    const QString uuid = QStringLiteral("cold-host");

    AppsSpy apps(&manager);
    manager.refreshApps(uuid);
    REQUIRE(apps.hosts.size() == 1);
    CHECK(apps.hosts.at(0) == uuid);
    CHECK(manager.apps(uuid).isEmpty());

    const auto inputs = manager.uiInputs(uuid, QString());
    CHECK(inputs.appsFailed);
    CHECK_FALSE(inputs.appsRead);
    CHECK(inputs.appCount == 0);

    // A refusal is FAILED, never EMPTY: on a paired host the two render
    // different copy, and only one of them is a fact about the host.
    moonlight::SessionUiInputs asPaired = inputs;
    asPaired.probeAttempted = true;
    asPaired.probeAnswered = true;
    asPaired.paired = true;
    asPaired.remembered = true;
    CHECK(moonlight::sessionUiState(asPaired) == moonlight::SessionUiState::AppsFailed);
}

TEST_CASE("the remembered app seeds the next session, not the binding", "[moonlight][binding]") {
    auto settings = test::makeSharedSettings();
    repository::MoonlightHostRepository repo(settings);
    repo.upsert(pairedHost());

    MoonlightManager manager(settings);
    const QString uuid = QStringLiteral("host-uuid");
    manager.setLastApp(uuid, QStringLiteral("1093255277"), QStringLiteral("Steam Big Picture"));

    REQUIRE(
        manager
            .bindController(QStringLiteral("a"), uuid, moonproto::kControllerTypeAuto, plainPad())
            .has_value());
    auto* session = manager.session(uuid);
    REQUIRE(session != nullptr);
    CHECK(session->appId() == QStringLiteral("1093255277"));
    CHECK(session->appName() == QStringLiteral("Steam Big Picture"));

    // The second binding JOINS that app; it never gets to pick again.
    REQUIRE(
        manager.bindController(QStringLiteral("b"), uuid, moonproto::kControllerTypePs, motionPad())
            .has_value());
    CHECK(manager.session(uuid)->appId() == QStringLiteral("1093255277"));
}
