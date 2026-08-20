// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The only ManifestGateway that ever touches a socket: test_update_checker
// drives a fake, so without this file nothing runs HttpGateways.cpp at all.
//
// classify() is pinned arm by arm because the reducer re-arms an Offline
// failure the moment reachability returns and makes an Http one wait out the
// backoff ladder — up to six hours. A bit moved between the two sides changes
// when a user who shut the lid sees the next check, and nothing else notices.
//
// The transport cases run against a loopback fixture server through the
// gateway's own setUrl() seam: a real QNetworkReply, no internet.

#include "update/HttpGateways.h"

#include "core/reducer/UpdateMachine.h"
#include "core/update/UpdateManifest.h"

#include <catch2/catch_test_macros.hpp>

#include <QAbstractSocket>
#include <QByteArray>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QString>
#include <QSysInfo>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#include <functional>
#include <memory>
#include <optional>
#include <utility>

using dish::reducer::UpdateError;
using dish::update::classify;
using dish::update::HttpManifestGateway;
using dish::update::ManifestFetchResult;
using dish::update::updateUserAgent;

namespace {

// A one-shot HTTP/1.1 origin on loopback. Each connection is answered by the
// responder the case supplies, which is what makes an abort mid-body and a
// half-sent response reachable at all.
class FixtureServer {
  public:
    using Responder = std::function<void(QTcpSocket*)>;

    explicit FixtureServer(Responder respond) : respond_(std::move(respond)) {
        listening_ = server_.listen(QHostAddress::LocalHost, 0);
        QObject::connect(&server_, &QTcpServer::newConnection, &server_, [this] { accept(); });
    }

    bool listening() const { return listening_; }

    QString url() const {
        return QStringLiteral("http://127.0.0.1:%1/latest.json").arg(server_.serverPort());
    }

  private:
    void accept() {
        QTcpSocket* sock = server_.nextPendingConnection();
        auto request = std::make_shared<QByteArray>();
        auto answered = std::make_shared<bool>(false);
        QObject::connect(sock, &QTcpSocket::readyRead, sock, [this, sock, request, answered] {
            request->append(sock->readAll());
            if (*answered || !request->contains("\r\n\r\n")) { return; }
            *answered = true;
            respond_(sock);
        });
        QObject::connect(sock, &QTcpSocket::disconnected, sock, &QObject::deleteLater);
    }

    QTcpServer server_;
    Responder respond_;
    bool listening_ = false;
};

QByteArray response(int status, const QByteArray& reason, const QByteArray& body) {
    return "HTTP/1.1 " + QByteArray::number(status) + " " + reason +
           "\r\nContent-Type: application/json\r\nContent-Length: " +
           QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
}

// The published example, trimmed to the required fields.
QByteArray goldenManifest() {
    return R"({"schema":1,"product":"dish-linux","version":"0.2.0","channel":"stable",)"
           R"("publishedAt":"2026-08-03T14:21:07Z","minimumSupportedVersion":"0.1.0"})";
}

FixtureServer::Responder canned(const QByteArray& bytes) {
    return [bytes](QTcpSocket* sock) {
        sock->write(bytes);
        sock->disconnectFromHost();
    };
}

// Headers promising 8 MiB, then a body that never ends: the gateway's own size
// cap is the only thing that can finish this fetch. Paced rather than written
// in one burst because QNetworkReply throttles downloadProgress to ~100 ms, and
// a single burst can land under one emission.
FixtureServer::Responder trickle() {
    return [](QTcpSocket* sock) {
        sock->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                    "Content-Length: 8388608\r\n\r\n");
        auto* pump = new QTimer(sock);
        QObject::connect(pump, &QTimer::timeout, sock, [sock] {
            if (sock->state() == QAbstractSocket::ConnectedState) {
                sock->write(QByteArray(24 * 1024, 'x'));
            }
        });
        pump->start(40);
    };
}

// Catch2 owns no event loop; spin the suite's QCoreApplication until the
// gateway answers, with a ceiling so a stall fails the case instead of hanging.
// Generous because only a FAILING case ever waits it out.
bool spinUntil(const std::function<bool()>& ready, int timeoutMs = 10000) {
    QElapsedTimer clock;
    clock.start();
    while (!ready() && clock.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    }
    return ready();
}

void spinFor(int ms) {
    spinUntil([] { return false; }, ms);
}

} // namespace

// ── classify ────────────────────────────────────────────────────────────────

TEST_CASE("classify: the five errors the switch names land on Offline", "[update][http]") {
    CHECK(classify(QNetworkReply::ConnectionRefusedError) == UpdateError::Offline);
    CHECK(classify(QNetworkReply::HostNotFoundError) == UpdateError::Offline);
    CHECK(classify(QNetworkReply::TemporaryNetworkFailureError) == UpdateError::Offline);
    CHECK(classify(QNetworkReply::NetworkSessionFailedError) == UpdateError::Offline);
    CHECK(classify(QNetworkReply::UnknownNetworkError) == UpdateError::Offline);
}

TEST_CASE("classify: a timeout and a proxy refusal are Http, not Offline", "[update][http]") {
    // Both read as "no internet" to a user and neither is named, so both wait
    // out the ladder instead of re-arming when the link returns.
    CHECK(classify(QNetworkReply::TimeoutError) == UpdateError::Http);
    CHECK(classify(QNetworkReply::ProxyConnectionRefusedError) == UpdateError::Http);
    CHECK(classify(QNetworkReply::ProxyTimeoutError) == UpdateError::Http);
}

TEST_CASE("classify: an answer from the far end, however bad, is Http", "[update][http]") {
    CHECK(classify(QNetworkReply::ContentNotFoundError) == UpdateError::Http);
    CHECK(classify(QNetworkReply::ContentAccessDenied) == UpdateError::Http);
    CHECK(classify(QNetworkReply::InternalServerError) == UpdateError::Http);
    CHECK(classify(QNetworkReply::TooManyRedirectsError) == UpdateError::Http);
    // A TLS failure is an interception or a stale root store, not a dead link.
    CHECK(classify(QNetworkReply::SslHandshakeFailedError) == UpdateError::Http);
    CHECK(classify(QNetworkReply::InsecureRedirectError) == UpdateError::Http);
}

TEST_CASE("classify: an aborted reply is Http", "[update][http]") {
    // Every abort lands here: the size cap, cancel(), and the request's own 30 s
    // transfer timeout — Qt reports that as OperationCanceled, never as
    // TimeoutError. So a link that goes quiet mid-request waits out the whole
    // ladder rather than re-arming when reachability returns.
    CHECK(classify(QNetworkReply::OperationCanceledError) == UpdateError::Http);
}

TEST_CASE("classify: NoError falls through to Http, never None", "[update][http]") {
    // Unreachable today — the handler checks error() first — but the default
    // arm means a caller that stopped checking would report a success as Http
    // rather than sail through with UpdateError::None.
    CHECK(classify(QNetworkReply::NoError) == UpdateError::Http);
}

// ── User-Agent ──────────────────────────────────────────────────────────────

TEST_CASE("updateUserAgent: is Dish/<version> (Linux; <running arch>)", "[update][http]") {
    const QRegularExpression shape(QStringLiteral(R"(^Dish/(\S+) \(Linux; ([^()]+)\)$)"));
    const QRegularExpressionMatch match = shape.match(updateUserAgent());
    REQUIRE(match.hasMatch());
    CHECK(match.captured(1) == QLatin1String(DISH_VERSION));
    // The arch used to be a hard-coded "x86_64", so every arm64 build lied
    // about itself to the release host. It is derived now.
    CHECK_FALSE(QSysInfo::currentCpuArchitecture().isEmpty());
    CHECK(match.captured(2) == QSysInfo::currentCpuArchitecture());
}

TEST_CASE("updateUserAgent: carries no identity and is safe as a raw header", "[update][http]") {
    const QString agent = updateUserAgent();
    // Stable across calls: a nonce in here would be a tracking id (PRIVACY.md 2.4).
    CHECK(agent == updateUserAgent());
    CHECK(agent.toUtf8() == agent.toLatin1()); // ASCII only
    CHECK_FALSE(agent.contains(QLatin1Char('\r')));
    CHECK_FALSE(agent.contains(QLatin1Char('\n')));
}

// ── Transport ───────────────────────────────────────────────────────────────

TEST_CASE("HttpManifestGateway: a 200 yields the parsed manifest and the exact bytes",
          "[update][http]") {
    FixtureServer server(canned(response(200, "OK", goldenManifest())));
    REQUIRE(server.listening());

    HttpManifestGateway gateway;
    gateway.setUrl(server.url());
    std::optional<ManifestFetchResult> result;
    gateway.fetch([&result](const ManifestFetchResult& r) { result = r; });

    REQUIRE(spinUntil([&result] { return result.has_value(); }));
    REQUIRE(result->manifest.has_value());
    CHECK(result->error == UpdateError::None);
    CHECK(result->manifest->version == QStringLiteral("0.2.0"));
    // Snapshotted verbatim beside the manifest so a support request can show
    // what the client was actually told.
    CHECK(result->body == goldenManifest());
}

TEST_CASE("HttpManifestGateway: a 404 in the publish window is a plain Http failure",
          "[update][http]") {
    FixtureServer server(canned(response(404, "Not Found", "{}")));
    REQUIRE(server.listening());

    HttpManifestGateway gateway;
    gateway.setUrl(server.url());
    std::optional<ManifestFetchResult> result;
    gateway.fetch([&result](const ManifestFetchResult& r) { result = r; });

    REQUIRE(spinUntil([&result] { return result.has_value(); }));
    CHECK(result->error == UpdateError::Http);
    CHECK_FALSE(result->manifest.has_value());
}

TEST_CASE("HttpManifestGateway: a captive portal's 200 splash is ManifestInvalid",
          "[update][http]") {
    FixtureServer server(canned(response(200, "OK", "<html>Sign in to continue</html>")));
    REQUIRE(server.listening());

    HttpManifestGateway gateway;
    gateway.setUrl(server.url());
    std::optional<ManifestFetchResult> result;
    gateway.fetch([&result](const ManifestFetchResult& r) { result = r; });

    REQUIRE(spinUntil([&result] { return result.has_value(); }));
    CHECK(result->error == UpdateError::ManifestInvalid);
    CHECK_FALSE(result->manifest.has_value());
}

TEST_CASE("HttpManifestGateway: a body past the cap is aborted mid-flight, never parsed",
          "[update][http]") {
    // The taxonomy is the evidence. An abort classifies as Http; a body that
    // arrived whole would instead reach UpdateManifest::parse and come back
    // ManifestInvalid (Oversize), which is the OTHER, independent cap. The
    // server never finishes the response, so nothing else can end this fetch.
    FixtureServer server(trickle());
    REQUIRE(server.listening());

    HttpManifestGateway gateway;
    gateway.setUrl(server.url());
    std::optional<ManifestFetchResult> result;
    gateway.fetch([&result](const ManifestFetchResult& r) { result = r; });

    REQUIRE(spinUntil([&result] { return result.has_value(); }));
    CHECK(result->error == UpdateError::Http);
    CHECK_FALSE(result->manifest.has_value());
    CHECK(result->body.isEmpty());
}

TEST_CASE("HttpManifestGateway: cancel drops the in-flight callback", "[update][http]") {
    FixtureServer server(canned(response(200, "OK", goldenManifest())));
    REQUIRE(server.listening());

    HttpManifestGateway gateway;
    gateway.setUrl(server.url());
    bool called = false;
    gateway.fetch([&called](const ManifestFetchResult&) { called = true; });
    // No event loop has run yet, so the reply is still in flight.
    gateway.cancel();

    spinFor(250);
    CHECK_FALSE(called);
}

TEST_CASE("HttpManifestGateway: a cancelled fetch never steals the next fetch's callback",
          "[update][http]") {
    // The contract the finished handler's stale-reply guard and cancel()'s
    // callback clear hold up between them. Only the contract is assertable:
    // abort() reports finished synchronously inside cancel(), so a superseded
    // reply never finishes late and the guard alone has no seam.
    FixtureServer server(canned(response(200, "OK", goldenManifest())));
    REQUIRE(server.listening());

    HttpManifestGateway gateway;
    gateway.setUrl(server.url());
    bool firstCalled = false;
    gateway.fetch([&firstCalled](const ManifestFetchResult&) { firstCalled = true; });
    gateway.cancel();

    std::optional<ManifestFetchResult> second;
    gateway.fetch([&second](const ManifestFetchResult& r) { second = r; });

    REQUIRE(spinUntil([&second] { return second.has_value(); }));
    REQUIRE(second->manifest.has_value());
    CHECK(second->manifest->version == QStringLiteral("0.2.0"));
    CHECK_FALSE(firstCalled);
}

TEST_CASE("HttpManifestGateway: a second fetch while one is in flight is ignored",
          "[update][http]") {
    FixtureServer server(canned(response(200, "OK", goldenManifest())));
    REQUIRE(server.listening());

    HttpManifestGateway gateway;
    gateway.setUrl(server.url());
    std::optional<ManifestFetchResult> first;
    gateway.fetch([&first](const ManifestFetchResult& r) { first = r; });
    bool secondCalled = false;
    gateway.fetch([&secondCalled](const ManifestFetchResult&) { secondCalled = true; });

    REQUIRE(spinUntil([&first] { return first.has_value(); }));
    CHECK(first->manifest.has_value());
    // Dropped on the floor, not queued: one callback per ACCEPTED fetch.
    spinFor(100);
    CHECK_FALSE(secondCalled);
}
