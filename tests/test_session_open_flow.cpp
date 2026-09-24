// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The connect PUT, driven through the real WifiConnectionManager against a
// satellite on loopback: a grant takes the connection live, and each refusal
// takes its own way out.
//
// test_session_manager.cpp pins the pure verdicts that choose between these
// arms. This pins what the manager does on each one.

#include "Network/ConnectionStore.h"
#include "Network/WifiConnection.h"
#include "Network/WifiConnectionManager.h"
#include "core/model/Protocol.h"
#include "core/reducer/ProtocolNegotiation.h"

#include "FakePairingListener.h"
#include "ManagerRig.h"

#include <catch2/catch_test_macros.hpp>

#include <QJsonObject>
#include <QString>

#include <functional>

using dish::net::ConnectIntent;
using dish::net::SessionState;
using dish::net::WifiConnection;
using dish::reducer::ProtocolCompat;
using dish::test::ManagerRig;
using dish::test::PairingAnswer;
using dish::test::SeenRequest;
using dish::test::spinFor;

namespace {

const QString kSessionPath = QStringLiteral("/api/connections");

// Every session PUT is answered with `status` and `body`; anything else is granted.
std::function<PairingAnswer(const SeenRequest&)> answeringSessions(int status, QJsonObject body) {
    return [status, body](const SeenRequest& r) {
        if (r.path == kSessionPath) { return PairingAnswer{status, body}; }
        return dish::test::grantingEverything(r);
    };
}

// With a key on file, a connect goes straight to the session PUT.
void keyed(ManagerRig& rig) {
    rig.store->setSharedKey(dish::test::kFixtureSharedKey, rig.server.id());
}

WifiConnection* connection(const ManagerRig& rig) { return rig.wifi->get(rig.server.id()); }

bool inState(const ManagerRig& rig, SessionState state) {
    const auto* conn = connection(rig);
    return conn != nullptr && conn->state() == state;
}

} // namespace

TEST_CASE("session open: a grant goes live on the satellite's connection id, and is remembered",
          "[wifi][session]") {
    ManagerRig rig;
    REQUIRE(rig.listener.listening());
    rig.listener.respond = dish::test::grantingEverything;
    keyed(rig);

    rig.wifi->connectTo(rig.server, ConnectIntent::UserInitiated);
    REQUIRE(spinFor([&] { return inState(rig, SessionState::Live); }));

    const auto* conn = connection(rig);
    CHECK(conn->connectionId() == QStringLiteral("c-1"));
    CHECK(conn->client() != nullptr);
    CHECK(conn->settledProtocolVersion() == dish::proto::kProtocolVersion);
    CHECK(conn->protocolCompat() == ProtocolCompat::Current);
    CHECK(rig.remembered());
    CHECK(rig.errors() == 0);

    const auto& put = rig.listener.requests().front();
    CHECK(put.method == "PUT");
    CHECK(put.path == kSessionPath);
    CHECK(put.body.value(QStringLiteral("deviceId")).toString() ==
          rig.store->getOrCreateDeviceId());
    CHECK(put.body.value(QStringLiteral("protocolVersion")).toInt() ==
          dish::proto::kProtocolVersion);
    CHECK_FALSE(put.body.value(QStringLiteral("hmacProof")).toString().isEmpty());
    // A fresh session reads the host's audio verdict again.
    CHECK(spinFor(
        [&] { return rig.listener.seen(QStringLiteral("/api/server/capabilities")) >= 1; }));
}

TEST_CASE("session open: material that does not decode never goes live", "[wifi][session]") {
    ManagerRig rig;
    REQUIRE(rig.listener.listening());
    rig.listener.respond = answeringSessions(
        200, QJsonObject{{QStringLiteral("connectionId"), QStringLiteral("c-1")},
                         {QStringLiteral("token"), QStringLiteral("zz")},
                         {QStringLiteral("sessionSalt"), QStringLiteral("0102030405060708")}});
    keyed(rig);

    rig.wifi->connectTo(rig.server, ConnectIntent::UserInitiated);
    REQUIRE(spinFor(
        [&] { return rig.listener.seen(kSessionPath) == 1 && inState(rig, SessionState::Idle); }));

    CHECK(connection(rig)->client() == nullptr);
    CHECK_FALSE(rig.remembered());
}

TEST_CASE("session open: a 401 drops the key and parks the connection", "[wifi][session]") {
    ManagerRig rig;
    REQUIRE(rig.listener.listening());
    rig.listener.respond =
        answeringSessions(401, QJsonObject{{QStringLiteral("code"), QStringLiteral("BAD_PROOF")}});
    keyed(rig);

    rig.wifi->connectTo(rig.server, ConnectIntent::UserInitiated);
    REQUIRE(spinFor([&] { return !rig.store->sharedKey(rig.server.id()).has_value(); }));

    CHECK(inState(rig, SessionState::Stale));
    // The user asked, so the user hears: pair again.
    CHECK(rig.errors() == 1);
}

TEST_CASE("session open: a 409 that still overlaps is offered again at the satellite's ceiling",
          "[wifi][session]") {
    ManagerRig rig;
    REQUIRE(rig.listener.listening());
    const int ceiling = dish::proto::kProtocolVersion - 1;
    REQUIRE(ceiling >= dish::proto::kProtocolVersionMin);
    rig.listener.respond = answeringSessions(
        409, QJsonObject{{QStringLiteral("supported"), ceiling},
                         {QStringLiteral("supportedMin"), dish::proto::kProtocolVersionMin}});
    keyed(rig);

    rig.wifi->connectTo(rig.server, ConnectIntent::UserInitiated);
    REQUIRE(spinFor([&] { return inState(rig, SessionState::Stale); }));
    CHECK(connection(rig)->offeredProtocolVersion() == ceiling);

    // The lowered offer sticks to the connection, so the next attempt does not repeat the number
    // that was just refused.
    rig.wifi->connectTo(rig.server, ConnectIntent::UserInitiated);
    REQUIRE(spinFor([&] { return rig.listener.seen(kSessionPath) == 2; }));
    CHECK(rig.listener.requests().back().body.value(QStringLiteral("protocolVersion")).toInt() ==
          ceiling);
}

TEST_CASE("session open: a 409 with no overlap says which end must update", "[wifi][session]") {
    ManagerRig rig;
    REQUIRE(rig.listener.listening());
    const int newer = dish::proto::kProtocolVersion + 1;
    rig.listener.respond =
        answeringSessions(409, QJsonObject{{QStringLiteral("supported"), newer + 1},
                                           {QStringLiteral("supportedMin"), newer}});
    keyed(rig);

    rig.wifi->connectTo(rig.server, ConnectIntent::UserInitiated);
    REQUIRE(spinFor([&] {
        const auto* conn = connection(rig);
        return conn != nullptr && conn->protocolCompat() == ProtocolCompat::DishUpdateRequired;
    }));

    CHECK(inState(rig, SessionState::Idle));
    CHECK(rig.errors() == 1);
    // Terminal: the key is fine, the versions are not.
    CHECK(rig.store->sharedKey(rig.server.id()).has_value());
}

TEST_CASE("session open: a changed certificate stops before the proof is sent, key kept",
          "[wifi][session]") {
    // The pin guards the old certificate, so no retry can succeed and only a Forget clears it.
    // Unlike a 401, nothing says the key is bad, so it stays.
    ManagerRig rig;
    REQUIRE(rig.listener.listening());
    rig.store->facade().pins().pin(rig.server.ip, QString(64, QLatin1Char('0')));
    rig.listener.respond = dish::test::grantingEverything;
    keyed(rig);

    rig.wifi->connectTo(rig.server, ConnectIntent::UserInitiated);
    REQUIRE(spinFor([&] { return rig.errors() == 1; }));

    CHECK(inState(rig, SessionState::Idle));
    CHECK(rig.listener.seen(kSessionPath) == 0);
    CHECK(rig.store->sharedKey(rig.server.id()).has_value());
}

TEST_CASE("session open: a silent reconnect to a dead port parks and says nothing",
          "[wifi][session]") {
    ManagerRig rig;
    rig.server.httpPort = dish::test::closedLoopbackPort();
    REQUIRE(rig.server.httpPort != 0);
    keyed(rig);

    rig.wifi->connectTo(rig.server, ConnectIntent::AutoReconnect);
    REQUIRE(spinFor([&] { return inState(rig, SessionState::Stale); }));

    CHECK(rig.errors() == 0);
    CHECK(rig.store->sharedKey(rig.server.id()).has_value());
}
