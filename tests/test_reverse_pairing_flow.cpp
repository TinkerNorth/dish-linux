// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Reverse pairing driven through the REAL WifiConnectionManager, against a
// satellite's pairing endpoint on loopback.
//
// test_reverse_pairing.cpp pins the decision core (nextReversePairingAction,
// the PIN formatter) as pure logic, and until this file that was all anyone
// could pin: the manager was called untestable, because its pairing client was
// reached through a process-wide verifier and the flow needs a real TLS peer.
// Both are now available, so the flow itself is asserted - what the manager
// sends, what each answer from the satellite does to the attempt, and that a
// changed certificate stops it before the PIN is written.
//
// The approval poll runs on the manager's own one-second timer, so the cases
// that reach it take a second or two.

#include "Network/WifiConnectionManager.h"

#include "FakePairingListener.h"
#include "ManagerRig.h"

#include <catch2/catch_test_macros.hpp>

#include <QJsonObject>
#include <QString>

#include <functional>

using dish::net::ReversePairingPhase;
using dish::test::ManagerRig;
using dish::test::PairingAnswer;
using dish::test::SeenRequest;
using dish::test::spinFor;

namespace {

// What the manager says for a satellite that does not answer, as WifiConnectionManager.cpp words
// it.
const QString kUnreachableMsg =
    QStringLiteral("Server unreachable — check it's powered on and on the same Wi-Fi.");

// The satellite's answers, by path: the first POST is the Path-B grant it stages for the operator,
// and every status poll gets `statusReply`.
std::function<PairingAnswer(const SeenRequest&)> satelliteAnswering(QJsonObject statusReply) {
    return [statusReply](const SeenRequest& r) {
        if (r.path == QStringLiteral("/api/pair")) {
            return PairingAnswer{
                200, QJsonObject{{QStringLiteral("ok"), false}, {QStringLiteral("pending"), true}}};
        }
        return PairingAnswer{200, statusReply};
    };
}

} // namespace

TEST_CASE("reverse pairing: the displayed PIN rides as the client PIN, with no operator PIN",
          "[reverse][flow]") {
    ManagerRig rig;
    REQUIRE(rig.listener.listening());
    rig.listener.respond =
        satelliteAnswering(QJsonObject{{QStringLiteral("status"), QStringLiteral("pending")}});

    rig.wifi->requestReversePairing(rig.server);
    // Until the reply has LANDED, not merely until the request arrived: the worker that sent it is
    // still inside the TLS stack in between, and a case that ends there leaves process exit racing
    // it, which hangs intermittently under load.
    REQUIRE(spinFor([&] { return !rig.wifi->isPairingInFlight(rig.server.id()); }));
    REQUIRE(rig.listener.seen(QStringLiteral("/api/pair")) == 1);

    const auto& post = rig.listener.requests().front();
    CHECK(post.method == "POST");
    // An empty operator PIN and the displayed one as clientPin: that is what selects Path B on the
    // satellite, where the operator approves the PIN this screen shows.
    CHECK(post.body.value(QStringLiteral("pin")).toString().isEmpty());
    CHECK(post.body.value(QStringLiteral("clientPin")).toString() == rig.wifi->reversePairingPin());
    CHECK(rig.wifi->reversePairingPin().size() == 4);
    CHECK(rig.wifi->reversePairingPhase() == ReversePairingPhase::AwaitingApproval);
    rig.wifi->cancelReversePairing();
}

TEST_CASE("reverse pairing: an approval on the poll stores the key and reports approved",
          "[reverse][flow]") {
    ManagerRig rig;
    REQUIRE(rig.listener.listening());
    rig.listener.respond = satelliteAnswering(
        QJsonObject{{QStringLiteral("status"), QStringLiteral("approved")},
                    {QStringLiteral("sharedKey"), dish::test::kFixtureSharedKey}});

    rig.wifi->requestReversePairing(rig.server);
    REQUIRE(spinFor(
        [&] { return rig.wifi->reversePairingPhase() == ReversePairingPhase::Approved; }, 8000));

    CHECK(rig.listener.seen(QStringLiteral("/api/pair/status")) >= 1);
    // The key the operator's approval released is the one the session will be keyed with.
    CHECK(rig.store->sharedKey(rig.server.id()) == dish::test::kFixtureSharedKey);
}

TEST_CASE("reverse pairing: a denial on the poll ends the attempt as declined", "[reverse][flow]") {
    ManagerRig rig;
    REQUIRE(rig.listener.listening());
    rig.listener.respond =
        satelliteAnswering(QJsonObject{{QStringLiteral("status"), QStringLiteral("denied")}});

    rig.wifi->requestReversePairing(rig.server);
    REQUIRE(spinFor(
        [&] { return rig.wifi->reversePairingPhase() == ReversePairingPhase::Declined; }, 8000));

    CHECK_FALSE(rig.store->sharedKey(rig.server.id()).has_value());
}

TEST_CASE("reverse pairing: a changed certificate ends it before the PIN is written",
          "[reverse][flow]") {
    // The host is pinned to a certificate the listener does not present, which is exactly what a
    // replaced machine behind a remembered address looks like. The gate on the TLS `encrypted` edge
    // must stop the POST, so the PIN never reaches a box this client cannot authenticate.
    ManagerRig rig;
    REQUIRE(rig.listener.listening());
    // A key on file: the pin guards a pairing. A pin with none behind it is trusted again on a
    // changed certificate (test_session_open_flow's pin-without-pairing case).
    rig.store->setSharedKey(dish::test::kFixtureSharedKey, rig.server.id());
    rig.store->facade().pins().pin(rig.server.ip, QString(64, QLatin1Char('0')));
    rig.listener.respond =
        satelliteAnswering(QJsonObject{{QStringLiteral("status"), QStringLiteral("pending")}});

    rig.wifi->requestReversePairing(rig.server);
    REQUIRE(spinFor(
        [&] { return rig.wifi->reversePairingPhase() == ReversePairingPhase::IdentityChanged; }));

    // IdentityChanged, not TimedOut: a changed identity, not a dead link, and not Declined, which
    // would offer a new code that cannot help. Whether the listener counted a handshake is not
    // asserted; under TLS 1.3 the client can abort before the server finishes its side.
    CHECK(rig.listener.seen(QStringLiteral("/api/pair")) == 0);
    CHECK(rig.listener.seen(QStringLiteral("/api/pair/status")) == 0);
    // Unlike a 401, a changed identity says nothing about the key, which stays.
    CHECK(rig.store->sharedKey(rig.server.id()) == dish::test::kFixtureSharedKey);
}

TEST_CASE("reverse pairing: a satellite on another protocol version ends it on the version, not "
          "a decline",
          "[reverse][flow]") {
    ManagerRig rig;
    REQUIRE(rig.listener.listening());
    rig.listener.respond = [](const SeenRequest&) {
        return PairingAnswer{409,
                             QJsonObject{{QStringLiteral("ok"), false},
                                         {QStringLiteral("error"), QStringLiteral("protocol")}}};
    };

    rig.wifi->requestReversePairing(rig.server);
    REQUIRE(spinFor(
        [&] { return rig.wifi->reversePairingPhase() == ReversePairingPhase::VersionMismatch; }));

    CHECK(rig.errors() == 1);
    CHECK_FALSE(rig.store->sharedKey(rig.server.id()).has_value());
}

TEST_CASE("reverse pairing: a satellite nobody answers at is unreachable, in the user's words",
          "[reverse][flow]") {
    ManagerRig rig;
    rig.server.pairPort = dish::test::closedLoopbackPort();

    rig.wifi->requestReversePairing(rig.server);
    REQUIRE(
        spinFor([&] { return rig.wifi->reversePairingPhase() == ReversePairingPhase::TimedOut; }));

    REQUIRE(rig.errors() == 1);
    // The transport's own words ("connect failed") are not a sentence for a user.
    CHECK(rig.events.back().message == kUnreachableMsg);
}
