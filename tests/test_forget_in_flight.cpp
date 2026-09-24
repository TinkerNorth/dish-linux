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
using dish::net::WifiConnection;
using dish::test::ManagerRig;
using dish::test::PairingAnswer;
using dish::test::SeenRequest;
using dish::test::spinFor;

namespace {

const QString kSharedKey =
    QStringLiteral("a1b2c3d4e5f6a7b8c9d0e1f2a3b4c5d6e7f8a9b0c1d2e3f4a5b6c7d8e9f0a1b2");

// A satellite that grants everything: a pair gets the key, a session PUT gets a session.
PairingAnswer grantingEverything(const SeenRequest& r) {
    if (r.path == QStringLiteral("/api/pair")) {
        return PairingAnswer{200, QJsonObject{{QStringLiteral("ok"), true},
                                              {QStringLiteral("sharedKey"), kSharedKey}}};
    }
    return PairingAnswer{
        200, QJsonObject{{QStringLiteral("connectionId"), QStringLiteral("c-1")},
                         {QStringLiteral("token"), QStringLiteral("0a0b0c0d")},
                         {QStringLiteral("sessionSalt"), QStringLiteral("0102030405060708")},
                         {QStringLiteral("epoch"), 1}}};
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

bool remembered(const ManagerRig& rig) {
    for (const auto& r : rig.wifi->remembered()) {
        if (r.id == rig.server.id()) { return true; }
    }
    return false;
}

} // namespace

TEST_CASE("forget in flight: a PIN pair's late key is not kept", "[wifi][forget]") {
    ManagerRig rig;
    REQUIRE(rig.listener.listening());
    rig.listener.respond = grantingEverything;
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
    rig.listener.respond = grantingEverything;
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
    rig.listener.respond = grantingEverything;
    rig.store->setSharedKey(kSharedKey, rig.server.id());
    rig.listener.hold();

    // A key on file, so the connect goes straight to the session PUT.
    rig.wifi->connectTo(rig.server, ConnectIntent::UserInitiated);
    forgetWhileInFlight(rig, QStringLiteral("/api/connections"));
    // No flag marks a session PUT in flight, so the reply is waited out instead.
    dish::test::settle(500);

    CHECK_FALSE(remembered(rig));
    CHECK_FALSE(rig.store->sharedKey(rig.server.id()).has_value());
    CHECK(rig.wifi->get(rig.server.id()) == nullptr);
}
