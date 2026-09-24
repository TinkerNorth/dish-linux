// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The pairing exchange as it crosses the wire, against a real TLS listener on
// loopback.
//
// test_pairing_outcome.cpp pins what a reply MEANS. This pins what the client
// SENDS, and the one property the exchange exists to have: the pinned-
// certificate check runs on the TLS `encrypted` edge, and a refusal aborts
// before a byte of the request - the PIN included - is written. That is the
// difference between trust-on-first-use and trust-on-every-use.
//
// The client and its verifier live on this thread, as they do in the manager,
// and so does the listener; each case spins the loop until the callback lands.

#include "Models/Models.h"
#include "Network/HTTPClient.h"
#include "Network/PairingOutcome.h"
#include "core/model/Protocol.h"

#include "FakePairingListener.h"

#include <catch2/catch_test_macros.hpp>

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QUrl>
#include <QUrlQuery>

#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

using dish::models::PairResponse;
using dish::net::HTTPClient;
using dish::net::PairingOutcome;
using dish::test::FakePairingListener;
using dish::test::PairingAnswer;
using dish::test::spinFor;

namespace {

// What a verifier was shown.
struct VerifierLog {
    std::vector<QString> hosts;
    std::vector<QByteArray> certs;
};

// A verifier that records what it was shown and answers `accept`; a refusal
// flags a mismatch only when `flagMismatch` says so, which is the difference
// between a changed certificate and a link that died.
HTTPClient::PinVerifier recordingVerifier(const std::shared_ptr<VerifierLog>& log, bool accept,
                                          bool flagMismatch = false) {
    return
        [log, accept, flagMismatch](const QString& host, const QByteArray& der, bool& pinMismatch) {
            log->hosts.push_back(host);
            log->certs.push_back(der);
            if (!accept && flagMismatch) { pinMismatch = true; }
            return accept;
        };
}

// One reply, as the callback delivered it.
struct Delivered {
    PairResponse response;
    bool pinMismatch = false;
};

// Sends one call and spins until its callback lands.
Delivered await(const std::function<void(HTTPClient::PairCb)>& send) {
    std::optional<Delivered> got;
    send([&got](const PairResponse& response, bool pinMismatch) {
        got = Delivered{response, pinMismatch};
    });
    REQUIRE(spinFor([&got] { return got.has_value(); }));
    return *got;
}

const QString kLoopback = QStringLiteral("127.0.0.1");

// The pair most cases send: device dev-1 with the operator's PIN, no client PIN.
Delivered pairDen(HTTPClient& client, int port) {
    return await([&](HTTPClient::PairCb cb) {
        client.pair(kLoopback, port, QStringLiteral("dev-1"), QStringLiteral("Den PC"),
                    QStringLiteral("1234"), QString(), std::move(cb));
    });
}

template <typename Arm> bool classifiesAs(const Delivered& reply) {
    return std::holds_alternative<Arm>(PairingOutcome::classify(reply.response, reply.pinMismatch));
}

} // namespace

TEST_CASE("pairing wire: pair posts the device, both pins, and the protocol version",
          "[pairing][wire]") {
    FakePairingListener listener;
    REQUIRE(listener.listening());
    HTTPClient client;
    client.setPinVerifier(recordingVerifier(std::make_shared<VerifierLog>(), true));

    const auto reply = pairDen(client, listener.port());

    REQUIRE(listener.requests().size() == 1);
    const auto& seen = listener.requests().front();
    CHECK(seen.method == "POST");
    CHECK(seen.path == QStringLiteral("/api/pair"));
    CHECK(seen.body.value(QStringLiteral("deviceId")).toString() == QStringLiteral("dev-1"));
    CHECK(seen.body.value(QStringLiteral("deviceName")).toString() == QStringLiteral("Den PC"));
    CHECK(seen.body.value(QStringLiteral("pin")).toString() == QStringLiteral("1234"));
    CHECK(seen.body.value(QStringLiteral("protocolVersion")).toInt() ==
          dish::proto::kProtocolVersion);
    // Both pins always ride in the body, empty when unused: the server tries a
    // valid operator pin first and only then the displayed one.
    CHECK(seen.body.contains(QStringLiteral("clientPin")));
    CHECK(seen.body.value(QStringLiteral("clientPin")).toString().isEmpty());

    CHECK(reply.response.reachable);
    CHECK_FALSE(reply.pinMismatch);
    CHECK(classifiesAs<PairingOutcome::Success>(reply));
}

TEST_CASE("pairing wire: the verifier sees the host dialled and the certificate presented",
          "[pairing][wire]") {
    FakePairingListener listener;
    REQUIRE(listener.listening());
    const auto log = std::make_shared<VerifierLog>();
    HTTPClient client;
    client.setPinVerifier(recordingVerifier(log, true));

    pairDen(client, listener.port());

    REQUIRE(log->hosts.size() == 1);
    // Keyed by the host the client dialled, which is what the pin store keys on.
    CHECK(log->hosts.front() == kLoopback);
    CHECK(log->certs.front() == listener.certDer());
}

TEST_CASE("pairing wire: a refusing verifier aborts before the PIN is written", "[pairing][wire]") {
    // The whole point of checking on the `encrypted` edge: the handshake has
    // completed, but the request - and the PIN in it - never goes out.
    FakePairingListener listener;
    REQUIRE(listener.listening());
    const auto log = std::make_shared<VerifierLog>();
    HTTPClient client;
    client.setPinVerifier(recordingVerifier(log, false, /*flagMismatch=*/true));

    const auto reply = pairDen(client, listener.port());
    dish::test::settle(200);

    // The refusal is the verifier's: it was shown the listener's certificate, which a dead port
    // would never have given it. How many handshakes the LISTENER counted is not asserted - under
    // TLS 1.3 the client can abort on its `encrypted` edge before the server has finished its
    // side, so the server may never queue the connection at all.
    CHECK(log->certs.size() == 1);
    CHECK(listener.requests().empty());
    CHECK_FALSE(reply.response.reachable);
    // A changed certificate, not a dead link: the flag is what tells them apart.
    CHECK(reply.pinMismatch);
    CHECK(classifiesAs<PairingOutcome::IdentityChanged>(reply));
}

TEST_CASE("pairing wire: a refusal that flags no mismatch reads as unreachable",
          "[pairing][wire]") {
    // The verifier alone decides whether a refusal is an identity change. One
    // that refuses without saying so must not be promoted into one here, or a
    // dead link would tell the user to forget and re-pair.
    FakePairingListener listener;
    REQUIRE(listener.listening());
    HTTPClient client;
    client.setPinVerifier(recordingVerifier(std::make_shared<VerifierLog>(), false));

    const auto reply = pairDen(client, listener.port());

    CHECK_FALSE(reply.response.reachable);
    CHECK_FALSE(reply.pinMismatch);
    CHECK(classifiesAs<PairingOutcome::Unreachable>(reply));
}

TEST_CASE("pairing wire: nothing listening is unreachable with no mismatch", "[pairing][wire]") {
    const auto log = std::make_shared<VerifierLog>();
    HTTPClient client;
    client.setPinVerifier(recordingVerifier(log, true));

    const auto reply = pairDen(client, dish::test::closedLoopbackPort());

    CHECK_FALSE(reply.response.reachable);
    CHECK(reply.response.httpStatus == 0);
    CHECK_FALSE(reply.pinMismatch);
    CHECK(classifiesAs<PairingOutcome::Unreachable>(reply));
    // No handshake, so nothing was ever shown to the verifier.
    CHECK(log->hosts.empty());
}

TEST_CASE("pairing wire: the transport status is stamped onto the parsed reply",
          "[pairing][wire]") {
    // The body cannot carry the HTTP status, and classify reads it: a 409 is a
    // protocol skew only because the status says so.
    FakePairingListener listener;
    REQUIRE(listener.listening());
    listener.respond = [](const dish::test::SeenRequest&) {
        return PairingAnswer{409,
                             QJsonObject{{QStringLiteral("ok"), false},
                                         {QStringLiteral("error"), QStringLiteral("protocol")}}};
    };
    HTTPClient client;
    client.setPinVerifier(recordingVerifier(std::make_shared<VerifierLog>(), true));

    const auto reply = pairDen(client, listener.port());

    CHECK(reply.response.httpStatus == 409);
    CHECK(classifiesAs<PairingOutcome::VersionMismatch>(reply));
}

TEST_CASE("pairing wire: the status poll percent-encodes the device id", "[pairing][wire]") {
    FakePairingListener listener;
    REQUIRE(listener.listening());
    listener.respond = [](const dish::test::SeenRequest&) {
        return PairingAnswer{200,
                             QJsonObject{{QStringLiteral("status"), QStringLiteral("pending")}}};
    };
    HTTPClient client;
    client.setPinVerifier(recordingVerifier(std::make_shared<VerifierLog>(), true));
    const QString awkwardId = QStringLiteral("a b&c=d");

    await([&](HTTPClient::PairCb cb) {
        client.pairStatus(kLoopback, listener.port(), awkwardId, std::move(cb));
    });

    REQUIRE(listener.requests().size() == 1);
    const auto& seen = listener.requests().front();
    CHECK(seen.method == "GET");
    CHECK(seen.path == QStringLiteral("/api/pair/status"));
    // One parameter, carrying the whole id: an unencoded `&` would have split it
    // into two, and an unencoded `=` would have cut the value short.
    CHECK(seen.query.queryItems(QUrl::FullyDecoded).size() == 1);
    CHECK(seen.query.queryItemValue(QStringLiteral("deviceId"), QUrl::FullyDecoded) == awkwardId);
}

TEST_CASE("pairing wire: the status poll's reply is read as an approval, not as a pair",
          "[pairing][wire]") {
    // The two routes answer in different shapes: a pair says ok/pending, a poll
    // says a status word. Read with the pair's parser, an approval would carry
    // no status at all and the operator's answer would never be seen.
    FakePairingListener listener;
    REQUIRE(listener.listening());
    listener.respond = [](const dish::test::SeenRequest&) {
        return PairingAnswer{200,
                             QJsonObject{{QStringLiteral("status"), QStringLiteral("approved")},
                                         {QStringLiteral("sharedKey"), QStringLiteral("0a0b")}}};
    };
    HTTPClient client;
    client.setPinVerifier(recordingVerifier(std::make_shared<VerifierLog>(), true));

    const auto reply = await([&](HTTPClient::PairCb cb) {
        client.pairStatus(kLoopback, listener.port(), QStringLiteral("dev-1"), std::move(cb));
    });

    CHECK(reply.response.reachable);
    CHECK(reply.response.httpStatus == 200);
    CHECK(reply.response.status == QStringLiteral("approved"));
    CHECK(reply.response.sharedKey == QStringLiteral("0a0b"));
}

TEST_CASE("pairing wire: each client runs the verifier it was built with", "[pairing][wire]") {
    // Two managers over two pin stores must each consult their own, which a
    // process-wide verifier cannot do.
    FakePairingListener listener;
    REQUIRE(listener.listening());
    const auto first = std::make_shared<VerifierLog>();
    const auto second = std::make_shared<VerifierLog>();
    HTTPClient a;
    a.setPinVerifier(recordingVerifier(first, true));
    HTTPClient b;
    b.setPinVerifier(recordingVerifier(second, false, /*flagMismatch=*/true));

    const auto fromA = await([&](HTTPClient::PairCb cb) {
        a.pair(kLoopback, listener.port(), QStringLiteral("dev-a"), QStringLiteral("A"),
               QStringLiteral("1111"), QString(), std::move(cb));
    });
    const auto fromB = await([&](HTTPClient::PairCb cb) {
        b.pair(kLoopback, listener.port(), QStringLiteral("dev-b"), QStringLiteral("B"),
               QStringLiteral("2222"), QString(), std::move(cb));
    });

    CHECK(fromA.response.reachable);
    CHECK_FALSE(fromA.pinMismatch);
    CHECK(fromB.pinMismatch);
    // Only A's request reached the listener; B's verifier stopped B's.
    REQUIRE(listener.requests().size() == 1);
    CHECK(listener.requests().front().body.value(QStringLiteral("deviceId")).toString() ==
          QStringLiteral("dev-a"));
    CHECK(first->hosts.size() == 1);
    CHECK(second->hosts.size() == 1);
}
