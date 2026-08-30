// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Pairing against a host that answers: the five phases over the wire, the two
// records a success has to leave behind, the wait a human needs in the middle
// of it, and every way it can end without one.
//
// WHY THE WHOLE HANDSHAKE AND NOT THE CRYPTO ALONE. test_moonlight_pairing
// drives the same algorithm directly and proves both ends agree byte for byte.
// It cannot see whether a pairing that succeeded is WRITTEN DOWN, and that is
// the failure this suite exists for: on the Android client a host that already
// trusted the device answered yes, the app announced success and stored nothing
// at all, so the row kept offering Pair and pressing it kept doing the same
// nothing. The only assertion that would have caught it reads the store after a
// handshake that really happened.
//
// The wait is here for the same reason. Phase 1 blocks on the host until a
// person walks over and types the code, so the request has to outlive the
// ordinary ten-second budget by a wide margin. A constant naming two minutes
// proves nothing on its own; what is asserted is that the request is still open
// long after the default would have killed it.

#include "MoonlightFakeHost.h"
#include "QSettingsFixture.h"
#include "core/moonlight/MoonlightSessionUi.h"
#include "repository/MoonlightHostRepository.h"
#include "source/moonlight/MoonlightHttp.h"
#include "source/moonlight/MoonlightManager.h"

#include <catch2/catch_test_macros.hpp>

#include <QElapsedTimer>
#include <QSslSocket>
#include <QString>

using namespace dish;
using namespace dish::source::moon;
using dish::test::FakeMoonlightHost;
using dish::test::settle;
using dish::test::spinFor;

namespace {

// The synthetic id a typed-in address carries. One host, one id, and on this
// client the address is it.
const QString kTypedId = QStringLiteral("addr:127.0.0.1");

bool tlsAvailable() { return QSslSocket::supportsSsl(); }

struct PairingOutcome {
    bool finished = false;
    bool ok = false;
    QString reason;
};

// Runs a pairing to whatever end it reaches and reports the token it finished
// with. An empty reason means it succeeded.
PairingOutcome pairAndSettle(MoonlightManager& manager, FakeMoonlightHost& host,
                             const QString& uuid, const QString& pinTyped = QString()) {
    PairingOutcome outcome;
    const auto token = QObject::connect(&manager, &MoonlightManager::pairingFinished,
                                        [&outcome](const QString&, bool ok, const QString& reason) {
                                            outcome.finished = true;
                                            outcome.ok = ok;
                                            outcome.reason = reason;
                                        });
    manager.pair(uuid);
    // The PIN is minted and shown before anything reaches the wire, so the
    // fixture can be told what the human sees without racing phase 1.
    host.typedPin = pinTyped.isEmpty() ? manager.pairingPin() : pinTyped;
    spinFor([&outcome] { return outcome.finished; });
    QObject::disconnect(token);
    return outcome;
}

// The fixture, typed in by address the way the add sheet does it.
void addFixture(MoonlightManager& manager, const FakeMoonlightHost& host) {
    manager.addManualHost(QStringLiteral("127.0.0.1"), QStringLiteral("Den"), host.httpPort(),
                          host.httpsPort());
}

} // namespace

TEST_CASE("a pairing that succeeds is written down, certificate and all",
          "[moonlight][pairing][wire]") {
    if (!tlsAvailable()) { SKIP("no TLS backend for the fixture host"); }
    FakeMoonlightHost host;
    REQUIRE(host.listening());

    auto settings = test::makeSharedSettings();
    test::seedClientIdentity(*settings);
    MoonlightManager manager(settings);
    addFixture(manager, host);

    const auto outcome = pairAndSettle(manager, host, kTypedId);

    REQUIRE(outcome.finished);
    CHECK(outcome.ok);
    CHECK(outcome.reason.isEmpty());
    // All five phases really happened: four in plaintext, and the last one on
    // the channel the pairing itself authorised.
    CHECK(host.seen(QStringLiteral("/pair")) == 5);

    // THE POINT OF THE CASE. Announcing success is not pairing; the record is.
    repository::MoonlightHostRepository repo(settings);
    const auto stored = repo.get(kTypedId);
    REQUIRE(stored.has_value());
    CHECK(stored->serverCertPem == host.certPem());
    CHECK(stored->paired());
    // And the row says so without anybody having to ask again: a handshake that
    // just completed IS the verification a probe would have gone looking for.
    const auto row = manager.row(kTypedId);
    REQUIRE(row.has_value());
    CHECK(row->paired);
    CHECK(row->trust == moonlight::HostTrust::Paired);
    CHECK(moonlight::hostTrust(manager.uiInputs(kTypedId, QString())) ==
          moonlight::HostTrust::Paired);
    CHECK_FALSE(manager.pairingActive());
}

TEST_CASE("a pairing outlives the timeout an ordinary call gets", "[moonlight][pairing][wire]") {
    if (!tlsAvailable()) { SKIP("no TLS backend for the fixture host"); }
    FakeMoonlightHost host;
    REQUIRE(host.listening());
    // The host has the request and is waiting for a human to type the code.
    host.pairStalls = true;

    auto settings = test::makeSharedSettings();
    test::seedClientIdentity(*settings);
    MoonlightManager manager(settings);
    addFixture(manager, host);

    bool finished = false;
    QObject::connect(&manager, &MoonlightManager::pairingFinished,
                     [&finished](const QString&, bool, const QString&) { finished = true; });
    manager.pair(kTypedId);
    REQUIRE(manager.pairingActive());
    REQUIRE(spinFor([&host] { return host.seen(QStringLiteral("/pair")) == 1; }));

    // Well past the default budget every other call gets. A phase 1 killed at
    // ten seconds takes the host's pending session with it, and the user is
    // told the code was wrong while they are still walking to the keyboard.
    QElapsedTimer clock;
    clock.start();
    spinFor([&finished] { return finished; }, MoonlightHttp::kDefaultTimeoutMs + 1500);
    CHECK(clock.elapsed() >= MoonlightHttp::kDefaultTimeoutMs);
    CHECK_FALSE(finished);
    CHECK(manager.pairingActive());
    static_assert(MoonlightHttp::kPairingTimeoutMs >= 120000,
                  "phase 1 is a human's window, not a network's");

    // Nothing is left dialling once the case ends.
    manager.cancelPairing();
    CHECK_FALSE(manager.pairingActive());
}

TEST_CASE("a pairing that fails names why and writes nothing", "[moonlight][pairing][wire]") {
    if (!tlsAvailable()) { SKIP("no TLS backend for the fixture host"); }

    SECTION("the code did not match") {
        FakeMoonlightHost host;
        REQUIRE(host.listening());
        auto settings = test::makeSharedSettings();
        test::seedClientIdentity(*settings);
        MoonlightManager manager(settings);
        addFixture(manager, host);

        // A digit out of place derives a different key on the host side, which
        // is the whole of what a mistyped PIN is.
        const auto outcome = pairAndSettle(manager, host, kTypedId, QStringLiteral("0000"));

        REQUIRE(outcome.finished);
        CHECK_FALSE(outcome.ok);
        CHECK(outcome.reason == QStringLiteral("wrongPin"));
        CHECK(manager.pairingRefusedReason(kTypedId) == QStringLiteral("wrongPin"));
        CHECK(moonlight::sessionUiState(manager.uiInputs(kTypedId, QString())) ==
              moonlight::SessionUiState::PairingRefused);

        // HALF A PAIRING IS NOT A PAIRING. The row the address wrote is still
        // there and still carries no anchor, so nothing later reads it as
        // trust.
        repository::MoonlightHostRepository repo(settings);
        const auto stored = repo.get(kTypedId);
        REQUIRE(stored.has_value());
        CHECK(stored->serverCertPem.isEmpty());
        CHECK_FALSE(stored->paired());
        CHECK(manager.row(kTypedId)->trust == moonlight::HostTrust::NotPaired);

        // And it can be tried again from where the user is standing.
        manager.pair(kTypedId);
        CHECK(manager.pairingActive());
        CHECK(manager.pairingPin().size() == 4);
        CHECK_FALSE(manager.pairingRefused(kTypedId));
        manager.cancelPairing();
    }

    SECTION("the host turned it down") {
        FakeMoonlightHost host;
        REQUIRE(host.listening());
        host.pairDeclines = true;
        auto settings = test::makeSharedSettings();
        test::seedClientIdentity(*settings);
        MoonlightManager manager(settings);
        addFixture(manager, host);

        const auto outcome = pairAndSettle(manager, host, kTypedId);

        REQUIRE(outcome.finished);
        CHECK_FALSE(outcome.ok);
        // A refusal and a wrong code want different advice, so they are
        // different tokens rather than one indistinguishable failure.
        CHECK(outcome.reason == QStringLiteral("declined"));
        repository::MoonlightHostRepository repo(settings);
        CHECK(repo.get(kTypedId)->serverCertPem.isEmpty());
    }

    SECTION("the user withdrew the question") {
        FakeMoonlightHost host;
        REQUIRE(host.listening());
        host.pairStalls = true;
        auto settings = test::makeSharedSettings();
        test::seedClientIdentity(*settings);
        MoonlightManager manager(settings);
        addFixture(manager, host);

        manager.pair(kTypedId);
        REQUIRE(spinFor([&host] { return host.seen(QStringLiteral("/pair")) == 1; }));
        manager.cancelPairing();
        settle();

        CHECK_FALSE(manager.pairingActive());
        CHECK(manager.pairingPin().isEmpty());
        // A cancel is not a refusal: there is nothing to tell the user off for.
        CHECK_FALSE(manager.pairingRefused(kTypedId));
        repository::MoonlightHostRepository repo(settings);
        CHECK(repo.get(kTypedId)->serverCertPem.isEmpty());
    }
}

TEST_CASE("a host typed in by address is asked what it is", "[moonlight][pairing][wire]") {
    FakeMoonlightHost host;
    REQUIRE(host.listening());
    host.uniqueId = kTypedId;
    host.pairStatus = 0;

    auto settings = test::makeSharedSettings();
    test::seedClientIdentity(*settings);
    MoonlightManager manager(settings);

    bool probed = false;
    QObject::connect(&manager, &MoonlightManager::probeFinished,
                     [&probed](const QString& id) { probed = id == kTypedId; });
    addFixture(manager, host);

    // TYPING AN ADDRESS IS A QUESTION. A row that appears without one is a
    // claim nobody checked: the hosts screen probes the rows it already has
    // when it opens, so a row added afterwards is the one nothing would ask.
    REQUIRE(spinFor([&probed] { return probed; }));
    CHECK(host.seen(QStringLiteral("/serverinfo")) == 1);
    const auto inputs = manager.uiInputs(kTypedId, QString());
    CHECK(inputs.probeAttempted);
    CHECK(inputs.probeAnswered);
    CHECK(moonlight::sessionUiState(inputs) == moonlight::SessionUiState::NotPaired);
}

TEST_CASE("a host typed in by address outlives the sweep and the process",
          "[moonlight][pairing][wire]") {
    FakeMoonlightHost host;
    REQUIRE(host.listening());

    auto settings = test::makeSharedSettings();
    {
        test::seedClientIdentity(*settings);
        MoonlightManager manager(settings);
        addFixture(manager, host);
        REQUIRE(manager.knows(kTypedId));

        // A SWEEP MERGES. One that answers with less than the last one, or with
        // nothing at all, must not delete a host the user typed in: a binding
        // pointing at it would be left naming nothing.
        manager.startDiscovery();
        REQUIRE(spinFor([&manager] { return !manager.isScanning(); }, 15000));
        CHECK(manager.knows(kTypedId));
        REQUIRE(manager.row(kTypedId).has_value());
        CHECK(manager.row(kTypedId)->address == QStringLiteral("127.0.0.1"));
    }

    // And it is on file rather than in memory, so a restart still has it.
    MoonlightManager next(settings);
    const auto row = next.row(kTypedId);
    REQUIRE(row.has_value());
    CHECK(row->name == QStringLiteral("Den"));
    repository::MoonlightHostRepository repo(settings);
    const auto stored = repo.get(kTypedId);
    REQUIRE(stored.has_value());
    CHECK(stored->httpPort == host.httpPort());
    CHECK(stored->httpsPort == host.httpsPort());
}
