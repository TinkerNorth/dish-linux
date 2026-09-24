// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The pairing exchange as it crosses the wire, against a real TLS listener on
// loopback.
//
// test_rest_control_plane.cpp pins what a reply MEANS (classify). Nothing pinned
// what the client SENDS, or the one property this exchange exists to have: the
// pinned-certificate check runs on the TLS `encrypted` edge and a refusal aborts
// before a byte of the request - the PIN included - is written. That is the
// difference between trust-on-first-use and trust-on-every-use, and it was only
// ever asserted by reading the code.
//
// Each case also carries the verifier it was built with. Until this file there
// was one verifier per process, set by whichever manager was built last, so a
// test could only ever see the last one.
//
// Every call runs on a worker thread, exactly as the manager runs it: the client
// blocks in a nested event loop and must never be called on the thread that owns
// the listener.

#include "Network/PairingClient.h"
#include "core/model/Protocol.h"

#include "FakePairingListener.h"

#include <catch2/catch_test_macros.hpp>

#include <QByteArray>
#include <QFuture>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslKey>
#include <QSslServer>
#include <QSslSocket>
#include <QString>
#include <QTcpServer>
#include <QUrl>
#include <QUrlQuery>
#include <QtConcurrent/QtConcurrentRun>

#include <functional>
#include <memory>
#include <mutex>
#include <vector>

using dish::net::PairingClient;
using dish::test::FakePairingListener;
using dish::test::PairingAnswer;
using dish::test::spinFor;

namespace {

// What a verifier was shown, from the worker thread it runs on.
struct VerifierLog {
    std::mutex mtx;
    std::vector<QString> hosts;
    std::vector<QByteArray> certs;
};

// A verifier that records what it was shown and answers `accept`; a refusal
// flags a mismatch only when `flagMismatch` says so, which is the difference
// between a changed certificate and a link that died.
PairingClient::PinVerifier recordingVerifier(const std::shared_ptr<VerifierLog>& log, bool accept,
                                             bool flagMismatch = false) {
    return
        [log, accept, flagMismatch](const QString& host, const QByteArray& der, bool& pinMismatch) {
            {
                const std::lock_guard<std::mutex> lock(log->mtx);
                log->hosts.push_back(host);
                log->certs.push_back(der);
            }
            if (!accept && flagMismatch) { pinMismatch = true; }
            return accept;
        };
}

// Runs one client call on a worker and spins this thread, which owns the
// listener, until it lands.
PairingClient::Reply onWorker(const std::function<PairingClient::Reply()>& call) {
    QFuture<PairingClient::Reply> future = QtConcurrent::run(call);
    REQUIRE(spinFor([&future] { return future.isFinished(); }));
    return future.result();
}

// A loopback port with nothing listening on it.
int closedPort() {
    QTcpServer probe;
    REQUIRE(probe.listen(QHostAddress::LocalHost, 0));
    const int port = static_cast<int>(probe.serverPort());
    probe.close();
    return port;
}

const QString kLoopback = QStringLiteral("127.0.0.1");

} // namespace

TEST_CASE("pairing wire: pair posts the device, both pins, and the protocol version",
          "[pairing][wire]") {
    FakePairingListener listener;
    REQUIRE(listener.listening());
    const PairingClient client(recordingVerifier(std::make_shared<VerifierLog>(), true));

    const auto reply = onWorker([&] {
        return client.pair(kLoopback, listener.port(), QStringLiteral("dev-1"),
                           QStringLiteral("Den PC"), QStringLiteral("1234"));
    });

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
    CHECK(std::holds_alternative<PairingClient::Success>(PairingClient::classify(reply.response)));
}

TEST_CASE("pairing wire: the verifier sees the host dialled and the certificate presented",
          "[pairing][wire]") {
    FakePairingListener listener;
    REQUIRE(listener.listening());
    const auto log = std::make_shared<VerifierLog>();
    const PairingClient client(recordingVerifier(log, true));

    onWorker([&] {
        return client.pair(kLoopback, listener.port(), QStringLiteral("dev-1"),
                           QStringLiteral("Den PC"), QStringLiteral("1234"));
    });

    const std::lock_guard<std::mutex> lock(log->mtx);
    REQUIRE(log->hosts.size() == 1);
    // Keyed by the host the client dialled, which is what the pin store keys on.
    CHECK(log->hosts.front() == kLoopback);
    CHECK(log->certs.front() == listener.certDer());
}

TEST_CASE("pairing wire: a refusing verifier aborts before the PIN is written", "[pairing][wire]") {
    // The whole point of checking on the `encrypted` edge. The handshake has
    // completed, so the listener saw a connection, but the request - and the PIN
    // in it - never went out.
    FakePairingListener listener;
    REQUIRE(listener.listening());
    const auto log = std::make_shared<VerifierLog>();
    const PairingClient client(recordingVerifier(log, false, /*flagMismatch=*/true));

    const auto reply = onWorker([&] {
        return client.pair(kLoopback, listener.port(), QStringLiteral("dev-1"),
                           QStringLiteral("Den PC"), QStringLiteral("1234"));
    });
    dish::test::settle(200);

    // The refusal is the verifier's: it was shown the listener's certificate, which a dead port
    // would never have given it. How many handshakes the LISTENER counted is not asserted - under
    // TLS 1.3 the client can abort on its `encrypted` edge before the server has finished its
    // side, so the server may never queue the connection at all.
    {
        const std::lock_guard<std::mutex> lock(log->mtx);
        CHECK(log->certs.size() == 1);
    }
    CHECK(listener.requests().empty());
    CHECK_FALSE(reply.response.reachable);
    // A changed certificate, not a dead link: the flag is what tells them apart.
    CHECK(reply.pinMismatch);
    CHECK(std::holds_alternative<PairingClient::IdentityChanged>(
        PairingClient::classify(reply.response, reply.pinMismatch)));
}

TEST_CASE("pairing wire: a refusal that flags no mismatch reads as unreachable",
          "[pairing][wire]") {
    // The verifier alone decides whether a refusal is an identity change. One
    // that refuses without saying so must not be promoted into one here, or a
    // dead link would tell the user to forget and re-pair.
    FakePairingListener listener;
    REQUIRE(listener.listening());
    const PairingClient client(recordingVerifier(std::make_shared<VerifierLog>(), false));

    const auto reply = onWorker([&] {
        return client.pair(kLoopback, listener.port(), QStringLiteral("dev-1"),
                           QStringLiteral("Den PC"), QStringLiteral("1234"));
    });

    CHECK_FALSE(reply.response.reachable);
    CHECK_FALSE(reply.pinMismatch);
    CHECK(std::holds_alternative<PairingClient::Unreachable>(
        PairingClient::classify(reply.response, reply.pinMismatch)));
}

TEST_CASE("pairing wire: nothing listening is unreachable with no mismatch", "[pairing][wire]") {
    const auto log = std::make_shared<VerifierLog>();
    const PairingClient client(recordingVerifier(log, true));
    const int port = closedPort();

    const auto reply = onWorker([&] {
        return client.pair(kLoopback, port, QStringLiteral("dev-1"), QStringLiteral("Den PC"),
                           QStringLiteral("1234"));
    });

    CHECK_FALSE(reply.response.reachable);
    CHECK_FALSE(reply.pinMismatch);
    // No handshake, so nothing was ever shown to the verifier.
    const std::lock_guard<std::mutex> lock(log->mtx);
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
    const PairingClient client(recordingVerifier(std::make_shared<VerifierLog>(), true));

    const auto reply = onWorker([&] {
        return client.pair(kLoopback, listener.port(), QStringLiteral("dev-1"),
                           QStringLiteral("Den PC"), QStringLiteral("1234"));
    });

    CHECK(reply.response.httpStatus == 409);
    CHECK(std::holds_alternative<PairingClient::VersionMismatch>(
        PairingClient::classify(reply.response)));
}

TEST_CASE("pairing wire: the status poll percent-encodes the device id", "[pairing][wire]") {
    FakePairingListener listener;
    REQUIRE(listener.listening());
    listener.respond = [](const dish::test::SeenRequest&) {
        return PairingAnswer{200,
                             QJsonObject{{QStringLiteral("status"), QStringLiteral("pending")}}};
    };
    const PairingClient client(recordingVerifier(std::make_shared<VerifierLog>(), true));
    const QString awkwardId = QStringLiteral("a b&c=d");

    onWorker([&] { return client.pairStatus(kLoopback, listener.port(), awkwardId); });

    REQUIRE(listener.requests().size() == 1);
    const auto& seen = listener.requests().front();
    CHECK(seen.method == "GET");
    CHECK(seen.path == QStringLiteral("/api/pair/status"));
    // One parameter, carrying the whole id: an unencoded `&` would have split it
    // into two, and an unencoded `=` would have cut the value short.
    CHECK(seen.query.queryItems(QUrl::FullyDecoded).size() == 1);
    CHECK(seen.query.queryItemValue(QStringLiteral("deviceId"), QUrl::FullyDecoded) == awkwardId);
}

TEST_CASE("pairing wire: each client runs the verifier it was built with", "[pairing][wire]") {
    // There used to be one verifier per process, replaced by every manager that
    // was built. Two clients over two pin stores must each consult their own.
    FakePairingListener listener;
    REQUIRE(listener.listening());
    const auto first = std::make_shared<VerifierLog>();
    const auto second = std::make_shared<VerifierLog>();
    const PairingClient a(recordingVerifier(first, true));
    const PairingClient b(recordingVerifier(second, false, /*flagMismatch=*/true));

    const auto fromA = onWorker([&] {
        return a.pair(kLoopback, listener.port(), QStringLiteral("dev-a"), QStringLiteral("A"),
                      QStringLiteral("1111"));
    });
    const auto fromB = onWorker([&] {
        return b.pair(kLoopback, listener.port(), QStringLiteral("dev-b"), QStringLiteral("B"),
                      QStringLiteral("2222"));
    });

    CHECK(fromA.response.reachable);
    CHECK_FALSE(fromA.pinMismatch);
    CHECK(fromB.pinMismatch);
    // Only A's request reached the listener; B's verifier stopped B's.
    REQUIRE(listener.requests().size() == 1);
    CHECK(listener.requests().front().body.value(QStringLiteral("deviceId")).toString() ==
          QStringLiteral("dev-a"));
    const std::lock_guard<std::mutex> lockA(first->mtx);
    const std::lock_guard<std::mutex> lockB(second->mtx);
    CHECK(first->hosts.size() == 1);
    CHECK(second->hosts.size() == 1);
}
