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
using dish::net::SessionState;
using dish::net::WifiConnection;
using dish::test::ManagerRig;
using dish::test::PairingAnswer;
using dish::test::SeenRequest;
using dish::test::spinFor;

namespace {

const QString kPairPath = QStringLiteral("/api/pair");
const QString kStatusPath = QStringLiteral("/api/pair/status");
const QString kSessionPath = QStringLiteral("/api/connections");
// Long enough for a reply the manager should do nothing with to have landed.
constexpr int kNothingFollowsMs = 500;

bool inState(const ManagerRig& rig, SessionState state) {
    const auto* conn = rig.wifi->get(rig.server.id());
    return conn != nullptr && conn->state() == state;
}

// A satellite that grants every pair and turns every session PUT away as shutting down.
PairingAnswer shuttingDownForSessions(const SeenRequest& r) {
    if (r.path == kSessionPath) { return PairingAnswer{503, QJsonObject{}}; }
    return dish::test::grantingEverything(r);
}

// A satellite whose operator approves the displayed PIN: the POST is staged for approval, the poll
// answers approved with the key, and anything after that is granted.
PairingAnswer approvingOnThePoll(const SeenRequest& r) {
    if (r.path == kPairPath) {
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
// forget() asked for, so whatever lands next lands after the connection is really gone.
void forgetOnceSeen(ManagerRig& rig, const QString& path) {
    REQUIRE(spinFor([&] { return rig.listener.seen(path) == 1; }));
    const QPointer<WifiConnection> conn = rig.wifi->get(rig.server.id());
    REQUIRE(conn);
    rig.wifi->forget(rig.server.id());
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    REQUIRE(conn.isNull());
}

// The forget above, and only then the held answer.
void forgetWhileInFlight(ManagerRig& rig, const QString& path) {
    forgetOnceSeen(rig, path);
    rig.listener.release();
}

// The forget above, and then the satellite paired afresh, so an answer released next lands on a
// connection it was not sent on while the fresh pair's own answer is still held.
// How many pairs the listener has been sent: the POSTs to the pairing path, not the DELETE a
// Forget with a key on file sends to the same path.
int pairsSent(const ManagerRig& rig) {
    int n = 0;
    for (const auto& r : rig.listener.requests()) {
        if (r.method == "POST" && r.path == kPairPath) { ++n; }
    }
    return n;
}

void forgetAndPairAgain(ManagerRig& rig, const QString& path) {
    forgetOnceSeen(rig, path);
    const int pairsBefore = pairsSent(rig);
    rig.wifi->pairWithPin(rig.server, QStringLiteral("2222"));
    REQUIRE(spinFor([&] { return pairsSent(rig) == pairsBefore + 1; }));
}

} // namespace

TEST_CASE("forget in flight: a PIN pair's late key is not kept", "[wifi][forget]") {
    ManagerRig rig;
    REQUIRE(rig.listener.listening());
    rig.listener.respond = dish::test::grantingEverything;
    rig.listener.hold();

    rig.wifi->pairWithPin(rig.server, QStringLiteral("1234"));
    forgetWhileInFlight(rig, kPairPath);
    REQUIRE(spinFor([&] { return !rig.wifi->isPairingInFlight(rig.server.id()); }));

    CHECK_FALSE(rig.store->sharedKey(rig.server.id()).has_value());
    CHECK(rig.wifi->get(rig.server.id()) == nullptr);
    // Keyed, it would have gone straight on to open a session.
    CHECK(rig.listener.seen(kSessionPath) == 0);
}

TEST_CASE("forget in flight: a connect's pair does not key the satellite again", "[wifi][forget]") {
    ManagerRig rig;
    REQUIRE(rig.listener.listening());
    rig.listener.respond = dish::test::grantingEverything;
    rig.listener.hold();

    // No key on file, so the connect starts with a PIN-less pair.
    rig.wifi->connectTo(rig.server, ConnectIntent::UserInitiated);
    forgetWhileInFlight(rig, kPairPath);
    REQUIRE(spinFor([&] { return !rig.wifi->isPairingInFlight(rig.server.id()); }));

    CHECK_FALSE(rig.store->sharedKey(rig.server.id()).has_value());
    CHECK(rig.wifi->get(rig.server.id()) == nullptr);
    CHECK(rig.listener.seen(kSessionPath) == 0);
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
    forgetWhileInFlight(rig, kSessionPath);
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
    CHECK(rig.listener.seen(kSessionPath) == 0);
    CHECK_FALSE(rig.remembered());
}

TEST_CASE("forget in flight: forgetting a satellite ends the approval request aimed at it",
          "[wifi][forget]") {
    // The Forget itself ends the attempt: a grant landing after it used to re-create the
    // satellite, keyed.
    ManagerRig rig;
    REQUIRE(rig.listener.listening());
    rig.listener.respond = dish::test::grantingEverything;
    rig.listener.hold();

    rig.wifi->requestReversePairing(rig.server);
    forgetWhileInFlight(rig, kPairPath);
    REQUIRE(spinFor([&] { return !rig.wifi->isPairingInFlight(rig.server.id()); }));

    CHECK(rig.wifi->reversePairingPhase() == ReversePairingPhase::Idle);
    CHECK_FALSE(rig.store->sharedKey(rig.server.id()).has_value());
    CHECK(rig.wifi->get(rig.server.id()) == nullptr);
    CHECK(rig.listener.seen(kSessionPath) == 0);
    CHECK_FALSE(rig.remembered());
}

TEST_CASE("forget in flight: forgetting a satellite drops the pin its address was given, "
          "remembered or not",
          "[wifi][forget][identity]") {
    // The store drops a pin through the remembered row, and a satellite turned away at its first
    // request was pinned by that request's handshake and never remembered.
    ManagerRig rig;
    REQUIRE(rig.listener.listening());
    rig.listener.respond = [](const SeenRequest&) {
        return PairingAnswer{200, QJsonObject{{QStringLiteral("ok"), false}}};
    };
    rig.wifi->requestReversePairing(rig.server);
    REQUIRE(spinFor([&] { return !rig.wifi->isPairingInFlight(rig.server.id()); }));
    REQUIRE(rig.store->facade().pins().pinnedFingerprint(rig.server.ip).has_value());
    REQUIRE_FALSE(rig.remembered());

    rig.wifi->forget(rig.server.id());

    CHECK_FALSE(rig.store->facade().pins().pinnedFingerprint(rig.server.ip).has_value());
}

// ---- A reply from before a Forget, landing while a fresh pair's connection holds the id ----

TEST_CASE("forget in flight: a session PUT from before a Forget leaves the connection a fresh "
          "pair made alone",
          "[wifi][forget]") {
    ManagerRig rig;
    REQUIRE(rig.listener.listening());
    rig.listener.respond = shuttingDownForSessions;
    rig.store->setSharedKey(dish::test::kFixtureSharedKey, rig.server.id());
    rig.listener.hold();

    // A key on file, so the connect goes straight to the session PUT.
    rig.wifi->connectTo(rig.server, ConnectIntent::UserInitiated);
    forgetAndPairAgain(rig, kSessionPath);
    rig.listener.releaseFirst("PUT", kSessionPath);
    dish::test::settle(kNothingFollowsMs);

    // The refusal was the old connection's to hear; the fresh pair is still out and still Linking.
    CHECK(inState(rig, SessionState::Linking));
    CHECK(rig.wifi->isPairingInFlight(rig.server.id()));
    CHECK(rig.errors() == 0);
}

TEST_CASE("forget in flight: a PIN pair's key from before a Forget does not key the connection a "
          "fresh pair made",
          "[wifi][forget]") {
    ManagerRig rig;
    REQUIRE(rig.listener.listening());
    rig.listener.respond = dish::test::grantingEverything;
    rig.listener.hold();

    rig.wifi->pairWithPin(rig.server, QStringLiteral("1111"));
    forgetAndPairAgain(rig, kPairPath);
    rig.listener.releaseFirst("POST", kPairPath);
    dish::test::settle(kNothingFollowsMs);

    // The fresh pair is still out, and the old answer neither ended it nor keyed it.
    CHECK(rig.wifi->isPairingInFlight(rig.server.id()));
    CHECK_FALSE(rig.store->sharedKey(rig.server.id()).has_value());
    CHECK(rig.listener.seen(kSessionPath) == 0);
    CHECK(inState(rig, SessionState::Linking));
}

TEST_CASE("forget in flight: a connect's pair from before a Forget does not key the connection a "
          "fresh pair made",
          "[wifi][forget]") {
    ManagerRig rig;
    REQUIRE(rig.listener.listening());
    rig.listener.respond = dish::test::grantingEverything;
    rig.listener.hold();

    // No key on file, so the connect starts with a PIN-less pair.
    rig.wifi->connectTo(rig.server, ConnectIntent::UserInitiated);
    forgetAndPairAgain(rig, kPairPath);
    rig.listener.releaseFirst("POST", kPairPath);
    dish::test::settle(kNothingFollowsMs);

    CHECK(rig.wifi->isPairingInFlight(rig.server.id()));
    CHECK_FALSE(rig.store->sharedKey(rig.server.id()).has_value());
    CHECK(rig.listener.seen(kSessionPath) == 0);
    CHECK(inState(rig, SessionState::Linking));
}

TEST_CASE("forget in flight: an approval request's reply from before a Forget leaves a fresh pair "
          "in flight",
          "[wifi][forget]") {
    ManagerRig rig;
    REQUIRE(rig.listener.listening());
    rig.listener.respond = dish::test::grantingEverything;
    rig.listener.hold();

    rig.wifi->requestReversePairing(rig.server);
    forgetAndPairAgain(rig, kPairPath);
    rig.listener.releaseFirst("POST", kPairPath);
    dish::test::settle(kNothingFollowsMs);

    CHECK(rig.wifi->isPairingInFlight(rig.server.id()));
    CHECK_FALSE(rig.store->sharedKey(rig.server.id()).has_value());
    CHECK(inState(rig, SessionState::Linking));
}

TEST_CASE("forget in flight: a 401 from before a Forget does not erase the key a fresh pair stored",
          "[wifi][forget]") {
    ManagerRig rig;
    REQUIRE(rig.listener.listening());
    int sessionPuts = 0;
    rig.listener.respond = [&sessionPuts](const SeenRequest& r) {
        // The first PUT, the one from before the Forget, is turned away as unpaired.
        const bool isTheFirstPut = r.path == kSessionPath && sessionPuts++ == 0;
        if (isTheFirstPut) {
            return PairingAnswer{
                401, QJsonObject{{QStringLiteral("code"), QStringLiteral("NOT_PAIRED")}}};
        }
        return dish::test::grantingEverything(r);
    };
    rig.store->setSharedKey(dish::test::kFixtureSharedKey, rig.server.id());
    rig.listener.hold();

    rig.wifi->connectTo(rig.server, ConnectIntent::UserInitiated);
    forgetAndPairAgain(rig, kSessionPath);
    rig.listener.releaseFirst("POST", kPairPath);
    REQUIRE(spinFor([&] { return rig.store->sharedKey(rig.server.id()).has_value(); }));
    rig.listener.releaseFirst("PUT", kSessionPath);
    dish::test::settle(kNothingFollowsMs);

    CHECK(rig.store->sharedKey(rig.server.id()).has_value());
    CHECK(rig.errors() == 0);
}
