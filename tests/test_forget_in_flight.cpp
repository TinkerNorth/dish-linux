// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Forgetting a satellite while one of its requests is still on the wire.
//
// forget() hands the connection to deleteLater, and the reply lands after it is
// gone. Each case holds the satellite's answer, forgets, runs the deferred
// delete, and only then releases the answer: whatever the reply says, it must
// not reach the freed connection, and it must not undo the forget by keying or
// remembering the satellite again.

#include "Network/ConnectionStore.h"
#include "Network/WifiConnection.h"
#include "Network/WifiConnectionManager.h"

#include "FakePairingListener.h"
#include "ManagerRig.h"

#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QEvent>
#include <QJsonObject>
#include <QPointer>
#include <QString>

using dish::net::ConnectIntent;
using dish::net::ReversePairingPhase;
using dish::net::WifiConnection;
using dish::test::ManagerRig;
using dish::test::PairingAnswer;
using dish::test::SeenRequest;
using dish::test::spinFor;

namespace {

const QString kStatusPath = QStringLiteral("/api/pair/status");

// A satellite whose operator approves the displayed PIN: the POST is staged for approval, the poll
// answers approved with the key, and anything after that is granted.
PairingAnswer approvingOnThePoll(const SeenRequest& r) {
    if (r.path == QStringLiteral("/api/pair")) {
        return PairingAnswer{
            200, QJsonObject{{QStringLiteral("ok"), false}, {QStringLiteral("pending"), true}}};
    }
    if (r.path == kStatusPath) {
        return PairingAnswer{
            200, QJsonObject{{QStringLiteral("status"), QStringLiteral("approved")},
                             {QStringLiteral("sharedKey"), dish::test::kFixtureSharedKey}}};
    }
    return dish::test::grantingEverything(r);
}

// Forgets the satellite once its request has reached the listener, then runs the deferred delete
// forget() asked for, so the answer released last lands after the connection is really gone.
void forgetWhileInFlight(ManagerRig& rig, const QString& path) {
    REQUIRE(spinFor([&] { return rig.listener.seen(path) == 1; }));
    const QPointer<WifiConnection> conn = rig.wifi->get(rig.server.id());
    REQUIRE(conn);
    rig.wifi->forget(rig.server.id());
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    REQUIRE(conn.isNull());
    rig.listener.release();
}

} // namespace

TEST_CASE("forget in flight: a PIN pair's late key is not kept", "[wifi][forget]") {
    ManagerRig rig;
    REQUIRE(rig.listener.listening());
    rig.listener.respond = dish::test::grantingEverything;
    rig.listener.hold();

    rig.wifi->pairWithPin(rig.server, QStringLiteral("1234"));
    forgetWhileInFlight(rig, QStringLiteral("/api/pair"));
    REQUIRE(spinFor([&] { return !rig.wifi->isPairingInFlight(rig.server.id()); }));

    CHECK_FALSE(rig.store->sharedKey(rig.server.id()).has_value());
    CHECK(rig.wifi->get(rig.server.id()) == nullptr);
    // Keyed, it would have gone straight on to open a session.
    CHECK(rig.listener.seen(QStringLiteral("/api/connections")) == 0);
}

TEST_CASE("forget in flight: a connect's pair does not key the satellite again", "[wifi][forget]") {
    ManagerRig rig;
    REQUIRE(rig.listener.listening());
    rig.listener.respond = dish::test::grantingEverything;
    rig.listener.hold();

    // No key on file, so the connect starts with a PIN-less pair.
    rig.wifi->connectTo(rig.server, ConnectIntent::UserInitiated);
    forgetWhileInFlight(rig, QStringLiteral("/api/pair"));
    REQUIRE(spinFor([&] { return !rig.wifi->isPairingInFlight(rig.server.id()); }));

    CHECK_FALSE(rig.store->sharedKey(rig.server.id()).has_value());
    CHECK(rig.wifi->get(rig.server.id()) == nullptr);
    CHECK(rig.listener.seen(QStringLiteral("/api/connections")) == 0);
}

TEST_CASE("forget in flight: a granted session does not remember the satellite again",
          "[wifi][forget]") {
    ManagerRig rig;
    REQUIRE(rig.listener.listening());
    rig.listener.respond = dish::test::grantingEverything;
    rig.store->setSharedKey(dish::test::kFixtureSharedKey, rig.server.id());
    rig.listener.hold();

    // A key on file, so the connect goes straight to the session PUT.
    rig.wifi->connectTo(rig.server, ConnectIntent::UserInitiated);
    forgetWhileInFlight(rig, QStringLiteral("/api/connections"));
    // No flag marks a session PUT in flight, so the reply is waited out instead.
    dish::test::settle(500);

    CHECK_FALSE(rig.remembered());
    CHECK_FALSE(rig.store->sharedKey(rig.server.id()).has_value());
    CHECK(rig.wifi->get(rig.server.id()) == nullptr);
}

TEST_CASE("forget in flight: an approval that lands after a forget does not bring the satellite "
          "back",
          "[wifi][forget]") {
    ManagerRig rig;
    REQUIRE(rig.listener.listening());
    rig.listener.respond = approvingOnThePoll;

    rig.wifi->requestReversePairing(rig.server);
    // Staged, so the approval poll starts on the manager's own one-second timer.
    REQUIRE(spinFor([&] { return !rig.wifi->isPairingInFlight(rig.server.id()); }));
    REQUIRE(rig.wifi->reversePairingPhase() == ReversePairingPhase::AwaitingApproval);
    rig.listener.hold();
    REQUIRE(spinFor([&] { return rig.listener.seen(kStatusPath) == 1; }));
    // Closing the sheet cancels the attempt, and only then can the user reach Forget.
    rig.wifi->cancelReversePairing();
    forgetWhileInFlight(rig, kStatusPath);
    // No flag marks a poll in flight, so the approval is waited out instead.
    dish::test::settle(500);

    CHECK_FALSE(rig.store->sharedKey(rig.server.id()).has_value());
    CHECK(rig.wifi->get(rig.server.id()) == nullptr);
    CHECK(rig.listener.seen(QStringLiteral("/api/connections")) == 0);
    CHECK_FALSE(rig.remembered());
}
