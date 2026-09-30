// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// MoonlightHttp's TLS calls against a real listener on loopback: the identity a host asks for is
// presented, the pin is checked before the request is written, whatever the host answers is handed
// over as it was said, and a host that sits on a call is let go at that call's own timeout. The
// listener is the satellite fake, which is all a GameStream host's HTTPS port is from here:
// something that speaks TLS and records what reached it.
//
// The whole pairing handshake, the plaintext phases included, is test_moonlight_pairing_wire's.

#include "source/moonlight/MoonlightHttp.h"

#include "FakePairingListener.h"
#include "FixtureIdentity.h"

#include <catch2/catch_test_macros.hpp>

#include <QByteArray>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSslCertificate>
#include <QString>
#include <QUrlQuery>

#include <optional>

using dish::source::moon::MoonlightHttp;
using dish::test::FakePairingListener;
using dish::test::PairingAnswer;
using dish::test::SeenRequest;
using dish::test::spinFor;

namespace {

const QString kLoopback = QStringLiteral("127.0.0.1");

QString hostCertPem() { return QString::fromStdString(dish::test::fixtureHostIdentity().certPem); }

QString clientCertPem() {
    return QString::fromStdString(dish::test::fixtureClientIdentity().certPem);
}

QByteArray derOf(const QString& pem) {
    return QSslCertificate::fromData(pem.toUtf8(), QSsl::Pem).value(0).toDer();
}

struct Answer {
    int status = -1;
    QByteArray body;
};

// The fixture's client identity, as a paired install carries it.
void carryClientIdentity(MoonlightHttp& http) {
    const auto& identity = dish::test::fixtureClientIdentity();
    http.setIdentity(QString::fromStdString(identity.certPem),
                     QString::fromStdString(identity.privateKeyPem),
                     QStringLiteral("7b5d0738cbb54d3e"));
}

// One TLS /serverinfo pinned to `pinnedPem`, spun until it lands.
Answer serverInfoFrom(MoonlightHttp& http, int port, const QString& pinnedPem,
                      int timeoutMs = MoonlightHttp::kDefaultTimeoutMs) {
    std::optional<Answer> got;
    http.getTls(
        kLoopback, port, QStringLiteral("/serverinfo"), QUrlQuery(), pinnedPem,
        [&got](int status, const QByteArray& body) { got = Answer{status, body}; }, timeoutMs);
    REQUIRE(spinFor([&got] { return got.has_value(); }));
    return *got;
}

} // namespace

TEST_CASE("moonlight http: a TLS call presents the identity the host asks for",
          "[moonlight][http][wire]") {
    FakePairingListener host;
    REQUIRE(host.listening());
    host.askForClientCertificates();
    MoonlightHttp http;
    carryClientIdentity(http);

    const Answer got = serverInfoFrom(http, host.port(), hostCertPem());

    CHECK(got.status == 200);
    REQUIRE(host.requests().size() == 1);
    // What a host's verify callback holds up against the device it paired.
    CHECK(host.requests().front().clientCertificate.toDer() == derOf(clientCertPem()));
}

TEST_CASE("moonlight http: a host presenting another certificate is sent nothing",
          "[moonlight][http][wire]") {
    FakePairingListener host;
    REQUIRE(host.listening());
    MoonlightHttp http;
    carryClientIdentity(http);

    // Pinned to a certificate this host does not hold: an imposter at the paired address.
    const Answer got = serverInfoFrom(http, host.port(), clientCertPem());
    dish::test::settle(200);

    CHECK(got.status == 0);
    CHECK(got.body.isEmpty());
    // Not the request, so not the key a launch carries in it either.
    CHECK(host.bytesReceived() == 0);
    CHECK(host.requests().empty());
}

TEST_CASE("moonlight http: the host's answer is handed over as it was said, refusals included",
          "[moonlight][http][wire]") {
    FakePairingListener host;
    REQUIRE(host.listening());
    // Wolf answers an unpaired HTTPS caller 401: trust lost, which is not a dead cable.
    const QJsonObject refusal{{QStringLiteral("status_code"), 401}};
    host.respond = [refusal](const SeenRequest&) { return PairingAnswer{401, refusal}; };
    MoonlightHttp http;
    carryClientIdentity(http);

    const Answer got = serverInfoFrom(http, host.port(), hostCertPem());

    CHECK(got.status == 401);
    CHECK(got.body == QJsonDocument(refusal).toJson(QJsonDocument::Compact));
}

TEST_CASE("moonlight http: a call the host sits on ends at its own timeout",
          "[moonlight][http][wire]") {
    FakePairingListener host;
    REQUIRE(host.listening());
    host.hold();
    MoonlightHttp http;
    carryClientIdentity(http);
    constexpr int kShortCallMs = 400;

    QElapsedTimer clock;
    clock.start();
    const Answer got = serverInfoFrom(http, host.port(), hostCertPem(), kShortCallMs);

    CHECK(got.status == 0);
    CHECK(host.requests().size() == 1);
    // The deadline is a coarse QTimer, which Qt lets fire up to 5% early.
    CHECK(clock.elapsed() >= kShortCallMs * 95 / 100);
    // Its own timeout, not the ten seconds every other call gets.
    CHECK(clock.elapsed() < MoonlightHttp::kDefaultTimeoutMs);
}

TEST_CASE("moonlight http: only the pinned certificate matches the pin", "[moonlight][http]") {
    const QByteArray hostDer = derOf(hostCertPem());
    REQUIRE_FALSE(hostDer.isEmpty());

    CHECK(MoonlightHttp::matchesPin(hostDer, hostCertPem()));
    // The same certificate written out again with other line ends is still the same one.
    CHECK(MoonlightHttp::matchesPin(hostDer, hostCertPem().replace('\n', QStringLiteral("\r\n"))));
    CHECK_FALSE(MoonlightHttp::matchesPin(hostDer, clientCertPem()));
    // Nothing pinned matches nothing, an empty certificate included.
    CHECK_FALSE(MoonlightHttp::matchesPin(QByteArray(), QString()));
}
