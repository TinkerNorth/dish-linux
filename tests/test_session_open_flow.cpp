// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The connect PUT, driven through the real WifiConnectionManager against a
// satellite on loopback: a grant takes the connection live, a grant this end
// cannot carry is handed back, and each refusal takes its own way out, the
// silent retry it may arm included.
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

#include <QHostAddress>
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
using dish::test::settle;
using dish::test::spinFor;

namespace {

const QString kSessionPath = QStringLiteral("/api/connections");
// The session grantingEverything hands out, where its release is addressed.
const QString kGrantedSessionPath = QStringLiteral("/api/connections/c-1");
// Past the first silent retry's backoff (reducer::backoffDelayMs(1), one second), with room.
constexpr int kPastFirstRetryMs = 2500;
// Long enough for a request the manager should not have sent to reach the listener.
constexpr int kNothingFollowsMs = 500;
// A UDP port the controller socket refuses, where the REST port is fine.
constexpr int kUnusableUdpPort = 0;

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

// A satellite the user connected to, now live and remembered.
void liveByUser(ManagerRig& rig) {
    rig.listener.respond = dish::test::grantingEverything;
    keyed(rig);
    rig.wifi->connectTo(rig.server, ConnectIntent::UserInitiated);
}

// How many times the granted session was handed back to the satellite.
int releasesOfTheGrant(const ManagerRig& rig) {
    int n = 0;
    for (const auto& r : rig.listener.requests()) {
        const bool isTheGrant = r.path == kGrantedSessionPath;
        const bool isARelease = r.method == "DELETE";
        if (isTheGrant && isARelease) { ++n; }
    }
    return n;
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

TEST_CASE("session open: material that does not decode never goes live, and is handed back",
          "[wifi][session]") {
    ManagerRig rig;
    REQUIRE(rig.listener.listening());
    rig.listener.respond = answeringSessions(
        200, QJsonObject{{QStringLiteral("connectionId"), QStringLiteral("c-1")},
                         {QStringLiteral("token"), QStringLiteral("zz")},
                         {QStringLiteral("sessionSalt"), QStringLiteral("0102030405060708")}});
    keyed(rig);

    rig.wifi->connectTo(rig.server, ConnectIntent::UserInitiated);
    REQUIRE(spinFor([&] { return rig.listener.seen(kSessionPath) == 1; }));
    REQUIRE(spinFor([&] { return releasesOfTheGrant(rig) == 1; }));

    CHECK(inState(rig, SessionState::Idle));
    CHECK(connection(rig)->client() == nullptr);
    CHECK_FALSE(rig.remembered());
    // The user asked, so the user hears that the link would not come up.
    CHECK(rig.errors() == 1);
}

TEST_CASE("session open: a grant whose controller socket will not open is handed back, and a "
          "user tap hears why",
          "[wifi][session]") {
    ManagerRig rig;
    rig.server.udpPort = kUnusableUdpPort;
    REQUIRE(rig.listener.listening());
    rig.listener.respond = dish::test::grantingEverything;
    keyed(rig);

    rig.wifi->connectTo(rig.server, ConnectIntent::UserInitiated);
    REQUIRE(spinFor([&] { return rig.listener.seen(kSessionPath) == 1; }));
    REQUIRE(spinFor([&] { return releasesOfTheGrant(rig) == 1; }));

    CHECK(inState(rig, SessionState::Idle));
    CHECK(connection(rig)->client() == nullptr);
    CHECK_FALSE(rig.remembered());
    CHECK(rig.errors() == 1);
}

TEST_CASE("session open: a silent grant whose socket will not open is handed back, without a "
          "word or a retry",
          "[wifi][session]") {
    ManagerRig rig;
    rig.server.udpPort = kUnusableUdpPort;
    REQUIRE(rig.listener.listening());
    rig.listener.respond = dish::test::grantingEverything;
    keyed(rig);

    rig.wifi->connectTo(rig.server, ConnectIntent::AutoReconnect);
    REQUIRE(spinFor([&] { return rig.listener.seen(kSessionPath) == 1; }));
    REQUIRE(spinFor([&] { return releasesOfTheGrant(rig) == 1; }));
    // A retry would be granted the same session and fail the same way.
    settle(kPastFirstRetryMs);

    CHECK(rig.listener.seen(kSessionPath) == 1);
    CHECK(inState(rig, SessionState::Idle));
    CHECK(rig.errors() == 0);
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

TEST_CASE("session retry: a disconnect cancels a silent retry still waiting out its backoff",
          "[wifi][session][retry]") {
    ManagerRig rig;
    REQUIRE(rig.listener.listening());
    rig.listener.respond = answeringSessions(503, QJsonObject{});
    keyed(rig);

    rig.wifi->connectTo(rig.server, ConnectIntent::AutoReconnect);
    REQUIRE(spinFor([&] { return inState(rig, SessionState::Stale); }));
    rig.wifi->disconnect(rig.server.id());
    settle(kPastFirstRetryMs);

    CHECK(rig.listener.seen(kSessionPath) == 1);
    CHECK(inState(rig, SessionState::Idle));
}

TEST_CASE("session retry: a silent failure after the disconnect still retries",
          "[wifi][session][retry]") {
    // The cancel is for the retries the disconnect ended, not for every later one: this is also
    // the order a session death takes, a disconnect and then its own retry.
    ManagerRig rig;
    REQUIRE(rig.listener.listening());
    rig.listener.respond = answeringSessions(503, QJsonObject{});
    keyed(rig);

    rig.wifi->connectTo(rig.server, ConnectIntent::AutoReconnect);
    REQUIRE(spinFor([&] { return inState(rig, SessionState::Stale); }));
    rig.wifi->disconnect(rig.server.id());
    rig.wifi->connectTo(rig.server, ConnectIntent::AutoReconnect);
    REQUIRE(spinFor(
        [&] { return rig.listener.seen(kSessionPath) == 2 && inState(rig, SessionState::Stale); }));

    CHECK(spinFor([&] { return rig.listener.seen(kSessionPath) == 3; }));
}

TEST_CASE("connect guard: an IPv6 satellite is refused before any request, and a user tap hears "
          "why",
          "[wifi][session][ipv6]") {
    // The satellite binds IPv4 only, for REST, the controller link and discovery alike, so an
    // IPv6 address could never reach it. The listener here answers on IPv6 loopback all the same.
    ManagerRig rig(QHostAddress::LocalHostIPv6);
    REQUIRE(rig.listener.listening());
    rig.listener.respond = dish::test::grantingEverything;
    keyed(rig);

    rig.wifi->connectTo(rig.server, ConnectIntent::UserInitiated);
    settle(kNothingFollowsMs);

    CHECK(rig.listener.requests().empty());
    CHECK(rig.errors() == 1);
    CHECK(connection(rig) == nullptr);
}

TEST_CASE("connect guard: a silent reconnect to an IPv6 satellite is refused without a word",
          "[wifi][session][ipv6]") {
    ManagerRig rig(QHostAddress::LocalHostIPv6);
    REQUIRE(rig.listener.listening());
    rig.listener.respond = dish::test::grantingEverything;
    keyed(rig);

    rig.wifi->connectTo(rig.server, ConnectIntent::AutoReconnect);
    settle(kNothingFollowsMs);

    CHECK(rig.listener.requests().empty());
    CHECK(rig.errors() == 0);
}

TEST_CASE("pairing guard: a PIN pair to an IPv6 satellite is refused before the PIN is sent",
          "[wifi][pairing][ipv6]") {
    ManagerRig rig(QHostAddress::LocalHostIPv6);
    REQUIRE(rig.listener.listening());
    rig.listener.respond = dish::test::grantingEverything;

    rig.wifi->pairWithPin(rig.server, QStringLiteral("1234"));
    settle(kNothingFollowsMs);

    CHECK(rig.listener.requests().empty());
    CHECK(rig.errors() == 1);
    CHECK_FALSE(rig.wifi->isPairingInFlight(rig.server.id()));
}

TEST_CASE("pairing guard: a reverse pair to an IPv6 satellite is refused before a PIN is shown",
          "[wifi][pairing][ipv6]") {
    ManagerRig rig(QHostAddress::LocalHostIPv6);
    REQUIRE(rig.listener.listening());
    rig.listener.respond = dish::test::grantingEverything;

    rig.wifi->requestReversePairing(rig.server);
    settle(kNothingFollowsMs);

    CHECK(rig.listener.requests().empty());
    CHECK(rig.errors() == 1);
    CHECK(rig.wifi->reversePairingPhase() == dish::net::ReversePairingPhase::Idle);
}

TEST_CASE("user disconnect: auto-reconnect leaves a satellite the user disconnected alone",
          "[wifi][session][hold]") {
    ManagerRig rig;
    REQUIRE(rig.listener.listening());
    liveByUser(rig);
    REQUIRE(spinFor([&] { return inState(rig, SessionState::Live); }));
    REQUIRE(rig.remembered());

    rig.wifi->disconnectByUser(rig.server.id());
    // The periodic sweep and every discovery scan both land here.
    rig.wifi->autoReconnectAll();
    settle(kNothingFollowsMs);

    CHECK(rig.listener.seen(kSessionPath) == 1);
    CHECK(inState(rig, SessionState::Idle));
    CHECK(rig.wifi->isHeldByUser(rig.server.id()));
}

TEST_CASE("user disconnect: a satellite dropped for any other reason still auto-reconnects",
          "[wifi][session][hold]") {
    // The plain disconnect is what a session death and a suspend take.
    ManagerRig rig;
    REQUIRE(rig.listener.listening());
    liveByUser(rig);
    REQUIRE(spinFor([&] { return inState(rig, SessionState::Live); }));

    rig.wifi->disconnect(rig.server.id());
    rig.wifi->autoReconnectAll();

    CHECK(spinFor(
        [&] { return rig.listener.seen(kSessionPath) == 2 && inState(rig, SessionState::Live); }));
    CHECK_FALSE(rig.wifi->isHeldByUser(rig.server.id()));
}

TEST_CASE("user disconnect: the user's next connect lifts the hold", "[wifi][session][hold]") {
    ManagerRig rig;
    REQUIRE(rig.listener.listening());
    liveByUser(rig);
    REQUIRE(spinFor([&] { return inState(rig, SessionState::Live); }));
    rig.wifi->disconnectByUser(rig.server.id());

    rig.wifi->connectTo(rig.server, ConnectIntent::UserInitiated);
    REQUIRE(spinFor([&] { return inState(rig, SessionState::Live); }));
    CHECK_FALSE(rig.wifi->isHeldByUser(rig.server.id()));

    // Back to ordinary: a drop the user did not ask for is reconnected again.
    rig.wifi->disconnect(rig.server.id());
    rig.wifi->autoReconnectAll();
    CHECK(spinFor([&] { return rig.listener.seen(kSessionPath) == 3; }));
}

TEST_CASE("user disconnect: pairing again lifts the hold", "[wifi][session][hold]") {
    ManagerRig rig;
    REQUIRE(rig.listener.listening());
    liveByUser(rig);
    REQUIRE(spinFor([&] { return inState(rig, SessionState::Live); }));
    rig.wifi->disconnectByUser(rig.server.id());

    rig.wifi->pairWithPin(rig.server, QStringLiteral("1234"));
    REQUIRE(spinFor([&] { return inState(rig, SessionState::Live); }));

    CHECK_FALSE(rig.wifi->isHeldByUser(rig.server.id()));
}

TEST_CASE("user disconnect: forget lifts the hold", "[wifi][session][hold]") {
    ManagerRig rig;
    REQUIRE(rig.listener.listening());
    liveByUser(rig);
    REQUIRE(spinFor([&] { return inState(rig, SessionState::Live); }));
    rig.wifi->disconnectByUser(rig.server.id());

    rig.wifi->forget(rig.server.id());
    CHECK_FALSE(rig.wifi->isHeldByUser(rig.server.id()));

    // The same satellite remembered again reconnects on its own like any other.
    keyed(rig);
    rig.store->remember(rig.server);
    rig.wifi->autoReconnectAll();
    CHECK(spinFor([&] { return rig.listener.seen(kSessionPath) == 2; }));
}

TEST_CASE("connect guard: a public address is refused before any request, and a user tap hears "
          "why",
          "[wifi][session][guard]") {
    ManagerRig rig;
    rig.server.ip = QStringLiteral("8.8.8.8");
    keyed(rig);

    rig.wifi->connectTo(rig.server, ConnectIntent::UserInitiated);

    CHECK(rig.errors() == 1);
    CHECK(connection(rig) == nullptr);
}

TEST_CASE("connect guard: a silent reconnect to a public address is refused without a word",
          "[wifi][session][guard]") {
    // A poisoned remembered entry is swept every 15 s: a line each time would be noise the user
    // cannot act on, and no background failure speaks.
    ManagerRig rig;
    rig.server.ip = QStringLiteral("8.8.8.8");
    keyed(rig);

    rig.wifi->connectTo(rig.server, ConnectIntent::AutoReconnect);
    rig.wifi->connectTo(rig.server, ConnectIntent::RetryAfterDeath);

    CHECK(rig.errors() == 0);
    CHECK(connection(rig) == nullptr);
}
