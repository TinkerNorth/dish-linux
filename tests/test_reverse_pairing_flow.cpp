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

#include "Network/ConnectionStore.h"
#include "Network/WifiConnectionManager.h"

#include "FakePairingListener.h"
#include "QSettingsFixture.h"

#include <catch2/catch_test_macros.hpp>

#include <QJsonObject>
#include <QSettings>
#include <QString>

#include <memory>

using dish::models::DiscoveredServer;
using dish::net::ConnectionStore;
using dish::net::ReversePairingPhase;
using dish::net::WifiConnectionManager;
using dish::test::FakePairingListener;
using dish::test::PairingAnswer;
using dish::test::SeenRequest;
using dish::test::spinFor;

namespace {

const QString kSharedKey =
    QStringLiteral("a1b2c3d4e5f6a7b8c9d0e1f2a3b4c5d6e7f8a9b0c1d2e3f4a5b6c7d8e9f0a1b2");

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

// The store, the manager, and the server that points at the listener. Built in the order AppModel
// builds them; the settings file outlives the store.
struct Rig {
    FakePairingListener listener;
    std::shared_ptr<QSettings> shared = dish::test::makeSharedSettings();
    std::unique_ptr<ConnectionStore> store;
    std::unique_ptr<WifiConnectionManager> wifi;
    DiscoveredServer server;

    Rig() {
        store = std::make_unique<ConnectionStore>(
            std::unique_ptr<QSettings>(new QSettings(shared->fileName(), QSettings::IniFormat)));
        wifi = std::make_unique<WifiConnectionManager>(store.get());
        server.machineId = QStringLiteral("m-reverse");
        server.ip = QStringLiteral("127.0.0.1");
        server.name = QStringLiteral("Den");
        server.pairPort = listener.port();
        server.httpPort = listener.port();
    }

    ReversePairingPhase phase() const { return wifi->reversePairingPhase(); }
};

} // namespace

TEST_CASE("reverse pairing: the displayed PIN rides as the client PIN, with no operator PIN",
          "[reverse][flow]") {
    Rig rig;
    REQUIRE(rig.listener.listening());
    rig.listener.respond =
        satelliteAnswering(QJsonObject{{QStringLiteral("status"), QStringLiteral("pending")}});

    rig.wifi->requestReversePairing(rig.server);
    REQUIRE(spinFor([&] { return rig.listener.seen(QStringLiteral("/api/pair")) >= 1; }));

    const auto& post = rig.listener.requests().front();
    CHECK(post.method == "POST");
    // An empty operator PIN and the displayed one as clientPin: that is what selects Path B on the
    // satellite, where the operator approves the PIN this screen shows.
    CHECK(post.body.value(QStringLiteral("pin")).toString().isEmpty());
    CHECK(post.body.value(QStringLiteral("clientPin")).toString() == rig.wifi->reversePairingPin());
    CHECK(rig.wifi->reversePairingPin().size() == 4);
    CHECK(rig.phase() == ReversePairingPhase::AwaitingApproval);
    rig.wifi->cancelReversePairing();
}

TEST_CASE("reverse pairing: an approval on the poll stores the key and reports approved",
          "[reverse][flow]") {
    Rig rig;
    REQUIRE(rig.listener.listening());
    rig.listener.respond =
        satelliteAnswering(QJsonObject{{QStringLiteral("status"), QStringLiteral("approved")},
                                       {QStringLiteral("sharedKey"), kSharedKey}});

    rig.wifi->requestReversePairing(rig.server);
    REQUIRE(spinFor([&] { return rig.phase() == ReversePairingPhase::Approved; }, 8000));

    CHECK(rig.listener.seen(QStringLiteral("/api/pair/status")) >= 1);
    // The key the operator's approval released is the one the session will be keyed with.
    CHECK(rig.store->sharedKey(rig.server.id()) == kSharedKey);
}

TEST_CASE("reverse pairing: a denial on the poll ends the attempt as declined", "[reverse][flow]") {
    Rig rig;
    REQUIRE(rig.listener.listening());
    rig.listener.respond =
        satelliteAnswering(QJsonObject{{QStringLiteral("status"), QStringLiteral("denied")}});

    rig.wifi->requestReversePairing(rig.server);
    REQUIRE(spinFor([&] { return rig.phase() == ReversePairingPhase::Declined; }, 8000));

    CHECK_FALSE(rig.store->sharedKey(rig.server.id()).has_value());
}

TEST_CASE("reverse pairing: a changed certificate ends it before the PIN is written",
          "[reverse][flow]") {
    // The host is pinned to a certificate the listener does not present, which is exactly what a
    // replaced machine behind a remembered address looks like. The gate on the TLS `encrypted` edge
    // must stop the POST, so the PIN never reaches a box this client cannot authenticate.
    Rig rig;
    REQUIRE(rig.listener.listening());
    rig.store->facade().pins().pin(rig.server.ip, QString(64, QLatin1Char('0')));
    rig.listener.respond =
        satelliteAnswering(QJsonObject{{QStringLiteral("status"), QStringLiteral("pending")}});

    rig.wifi->requestReversePairing(rig.server);
    REQUIRE(spinFor([&] { return rig.phase() == ReversePairingPhase::Declined; }));

    CHECK(rig.listener.handshakes() >= 1);
    CHECK(rig.listener.seen(QStringLiteral("/api/pair")) == 0);
    CHECK(rig.listener.seen(QStringLiteral("/api/pair/status")) == 0);
    CHECK_FALSE(rig.store->sharedKey(rig.server.id()).has_value());
}
