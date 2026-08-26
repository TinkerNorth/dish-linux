// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The RTSP transport against a loopback host that behaves the way a real one
// does: it answers exactly one message per TCP connection and then hangs up.
// Reusing the socket cost the whole stream setup once, failing at DESCRIBE with
// the host already gone, so the connection count is asserted and not merely the
// replies. The DESCRIBE reply's shape — no Content-length, framed by the close —
// is pinned here too, because nothing else in the suite reads a body to EOF.

#include "source/moonlight/MoonlightRtspClient.h"

#include <catch2/catch_test_macros.hpp>

#include <QByteArray>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QList>
#include <QTcpServer>
#include <QTcpSocket>

#include <functional>
#include <memory>
#include <optional>
#include <utility>

using dish::source::moon::MoonlightRtspClient;
namespace moonrtsp = dish::moonrtsp;

namespace {

// A loopback RTSP origin. Each connection is answered by the responder the case
// supplies, so a hang-up before answering and a body framed by the close are
// both reachable.
class FixtureHost {
  public:
    using Responder = std::function<void(QTcpSocket*, const QByteArray& request)>;

    explicit FixtureHost(Responder respond) : respond_(std::move(respond)) {
        listening_ = server_.listen(QHostAddress::LocalHost, 0);
        QObject::connect(&server_, &QTcpServer::newConnection, &server_, [this] { accept(); });
    }

    bool listening() const { return listening_; }
    int port() const { return static_cast<int>(server_.serverPort()); }
    int connections() const { return connections_; }
    const QList<QByteArray>& requests() const { return requests_; }

    // Stops accepting, so a client that reuses a socket cannot be rescued by a
    // second connection succeeding.
    void closeListener() { server_.close(); }

  private:
    void accept() {
        QTcpSocket* sock = server_.nextPendingConnection();
        ++connections_;
        auto request = std::make_shared<QByteArray>();
        auto answered = std::make_shared<bool>(false);
        QObject::connect(sock, &QTcpSocket::readyRead, sock, [this, sock, request, answered] {
            request->append(sock->readAll());
            if (*answered || !request->contains("\r\n\r\n")) { return; }
            *answered = true;
            requests_.append(*request);
            respond_(sock, *request);
        });
        QObject::connect(sock, &QTcpSocket::disconnected, sock, &QObject::deleteLater);
    }

    QTcpServer server_;
    Responder respond_;
    bool listening_ = false;
    int connections_ = 0;
    QList<QByteArray> requests_;
};

// Catch2 owns no event loop; spin the suite's QCoreApplication until the client
// answers, with a ceiling so a stall fails the case instead of hanging.
bool spinUntil(const std::function<bool()>& ready, int timeoutMs = 8000) {
    QElapsedTimer clock;
    clock.start();
    while (!ready() && clock.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    }
    return ready();
}

// One request, run to completion. `done` distinguishes "no reply yet" from
// "replied with nothing", which nullopt alone cannot.
struct Exchange {
    bool done = false;
    std::optional<moonrtsp::Response> response;
};

Exchange ask(MoonlightRtspClient& client, const QString& text, int timeoutMs = 8000) {
    auto exchange = std::make_shared<Exchange>();
    client.request(text, [exchange](const std::optional<moonrtsp::Response>& response) {
        exchange->response = response;
        exchange->done = true;
    });
    spinUntil([exchange] { return exchange->done; }, timeoutMs);
    return *exchange;
}

// A host that answers and then hangs up, exactly as Sunshine does.
FixtureHost::Responder answerAndHangUp(const QByteArray& reply) {
    return [reply](QTcpSocket* sock, const QByteArray&) {
        sock->write(reply);
        sock->flush();
        sock->disconnectFromHost();
    };
}

QByteArray okWithLength(const QByteArray& body) {
    return "RTSP/1.0 200 OK\r\nCSeq: 1\r\nSession: DEADBEEFCAFE;timeout = 90\r\n"
           "Content-length: " +
           QByteArray::number(body.size()) + "\r\n\r\n" + body;
}

} // namespace

TEST_CASE("every RTSP request gets its own connection", "[moonlight][rtspclient]") {
    // A Moonlight host answers exactly one message per TCP connection and then
    // hangs up on its own: a second message written into that socket is never
    // seen at all. Three requests must therefore be three connections.
    FixtureHost host(answerAndHangUp("RTSP/1.0 200 OK\r\nCSeq: 1\r\n"
                                     "Transport: server_port=47999\r\n"
                                     "X-SS-Connect-Data: 4270471497\r\n\r\n"));
    REQUIRE(host.listening());

    MoonlightRtspClient client;
    client.open(QStringLiteral("127.0.0.1"), host.port());

    for (int i = 0; i < 3; ++i) {
        const auto exchange = ask(client, QStringLiteral("OPTIONS rtsp://127.0.0.1:1 RTSP/1.0\r\n"
                                                         "CSeq: %1\r\n\r\n")
                                              .arg(i + 1));
        REQUIRE(exchange.done);
        REQUIRE(exchange.response.has_value());
        CHECK(exchange.response->ok());
        CHECK(moonrtsp::transportPort(*exchange.response) == 47999);
        // Above INT32_MAX, and it survives the whole transport path.
        CHECK(moonrtsp::connectData(*exchange.response) == 4270471497U);
    }
    CHECK(host.connections() == 3);
    CHECK(host.requests().size() == 3);
}

TEST_CASE("open() reports readiness without dialling", "[moonlight][rtspclient]") {
    FixtureHost host(answerAndHangUp("RTSP/1.0 200 OK\r\nCSeq: 1\r\n\r\n"));
    REQUIRE(host.listening());

    MoonlightRtspClient client;
    bool ready = false;
    QObject::connect(&client, &MoonlightRtspClient::connected, &client, [&ready] { ready = true; });
    client.open(QStringLiteral("127.0.0.1"), host.port());
    CHECK(spinUntil([&ready] { return ready; }));
    CHECK(client.isOpen());
    // Nothing has been dialled yet: the first socket is the first request's.
    CHECK(host.connections() == 0);

    REQUIRE(ask(client, QStringLiteral("OPTIONS * RTSP/1.0\r\nCSeq: 1\r\n\r\n")).done);
    CHECK(host.connections() == 1);
}

TEST_CASE("a reply with no Content-length is framed by the close", "[moonlight][rtspclient]") {
    // The DESCRIBE shape: the host sends the SDP with no length header at all
    // and simply closes, so the rest of the stream is the body.
    const QByteArray sdp = "a=fmtp:97 surround-params=21101\r\n"
                           "a=rtpmap:96 H264/90000\r\n"
                           "sprop-parameter-sets=AAAAAU\r\n";
    FixtureHost host([sdp](QTcpSocket* sock, const QByteArray&) {
        sock->write("RTSP/1.0 200 OK\r\nCSeq: 2\r\n\r\n");
        sock->flush();
        // Written in two pieces so a reader that stops at the first chunk is
        // caught rather than passing by luck.
        sock->write(sdp.left(20));
        sock->flush();
        sock->write(sdp.mid(20));
        sock->flush();
        sock->disconnectFromHost();
    });
    REQUIRE(host.listening());

    MoonlightRtspClient client;
    client.open(QStringLiteral("127.0.0.1"), host.port());
    const auto exchange = ask(client, QStringLiteral("DESCRIBE rtsp://127.0.0.1:1 RTSP/1.0\r\n"
                                                     "CSeq: 2\r\nAccept: application/sdp\r\n\r\n"));
    REQUIRE(exchange.done);
    REQUIRE(exchange.response.has_value());
    CHECK(exchange.response->ok());
    CHECK(exchange.response->cseq == 2);
    CHECK_FALSE(moonrtsp::contentLength(*exchange.response).has_value());
    // The WHOLE body, not the part that had arrived when the head did.
    CHECK(exchange.response->payload == sdp.toStdString());
}

TEST_CASE("a reply with Content-length does not wait for the close", "[moonlight][rtspclient]") {
    const QByteArray body = "SETUP-BODY";
    FixtureHost host([body](QTcpSocket* sock, const QByteArray&) {
        sock->write(okWithLength(body));
        sock->flush(); // and deliberately no hang-up
    });
    REQUIRE(host.listening());

    MoonlightRtspClient client;
    client.open(QStringLiteral("127.0.0.1"), host.port());
    const auto exchange = ask(client, QStringLiteral("SETUP streamid=audio/0/0 RTSP/1.0\r\n"
                                                     "CSeq: 3\r\n\r\n"));
    REQUIRE(exchange.done);
    REQUIRE(exchange.response.has_value());
    CHECK(exchange.response->ok());
    CHECK(moonrtsp::sessionId(*exchange.response) == "DEADBEEFCAFE");
    CHECK(exchange.response->payload == body.toStdString());
}

TEST_CASE("a body split across writes is not truncated at its length", "[moonlight][rtspclient]") {
    const QByteArray body = "0123456789abcdefghij";
    FixtureHost host([body](QTcpSocket* sock, const QByteArray&) {
        sock->write("RTSP/1.0 200 OK\r\nCSeq: 4\r\nContent-length: " +
                    QByteArray::number(body.size()) + "\r\n\r\n");
        sock->flush();
        sock->write(body.left(5));
        sock->flush();
        sock->write(body.mid(5));
        sock->flush();
        sock->disconnectFromHost();
    });
    REQUIRE(host.listening());

    MoonlightRtspClient client;
    client.open(QStringLiteral("127.0.0.1"), host.port());
    const auto exchange = ask(client, QStringLiteral("ANNOUNCE x RTSP/1.0\r\nCSeq: 4\r\n\r\n"));
    REQUIRE(exchange.done);
    REQUIRE(exchange.response.has_value());
    CHECK(exchange.response->payload == body.toStdString());
}

TEST_CASE("a host that hangs up before answering fails that step", "[moonlight][rtspclient]") {
    // The mid-handshake death this transport used to report as nothing at all.
    FixtureHost host([](QTcpSocket* sock, const QByteArray&) { sock->abort(); });
    REQUIRE(host.listening());

    MoonlightRtspClient client;
    client.open(QStringLiteral("127.0.0.1"), host.port());
    const auto exchange = ask(client, QStringLiteral("PLAY x RTSP/1.0\r\nCSeq: 5\r\n\r\n"));
    REQUIRE(exchange.done);
    CHECK_FALSE(exchange.response.has_value());
}

TEST_CASE("a half-sent head is not mistaken for a reply", "[moonlight][rtspclient]") {
    FixtureHost host([](QTcpSocket* sock, const QByteArray&) {
        sock->write("RTSP/1.0 200 OK\r\nCSeq: 6\r\nTransport: server_p");
        sock->flush();
        sock->disconnectFromHost();
    });
    REQUIRE(host.listening());

    MoonlightRtspClient client;
    client.open(QStringLiteral("127.0.0.1"), host.port());
    const auto exchange = ask(client, QStringLiteral("SETUP streamid=video/0/0 RTSP/1.0\r\n"
                                                     "CSeq: 6\r\n\r\n"));
    REQUIRE(exchange.done);
    CHECK_FALSE(exchange.response.has_value());
}

TEST_CASE("a non-RTSP reply is refused rather than misparsed", "[moonlight][rtspclient]") {
    FixtureHost host(answerAndHangUp("HTTP/1.1 200 OK\r\nContent-length: 0\r\n\r\n"));
    REQUIRE(host.listening());

    MoonlightRtspClient client;
    client.open(QStringLiteral("127.0.0.1"), host.port());
    const auto exchange = ask(client, QStringLiteral("OPTIONS * RTSP/1.0\r\nCSeq: 7\r\n\r\n"));
    REQUIRE(exchange.done);
    CHECK_FALSE(exchange.response.has_value());
}

TEST_CASE("a refusal reaches the caller with its status", "[moonlight][rtspclient]") {
    // The answer a minimal ANNOUNCE SDP earns from a real host.
    FixtureHost host(answerAndHangUp("RTSP/1.0 400 BAD REQUEST\r\nCSeq: 8\r\n\r\n"));
    REQUIRE(host.listening());

    MoonlightRtspClient client;
    client.open(QStringLiteral("127.0.0.1"), host.port());
    const auto exchange = ask(client, QStringLiteral("ANNOUNCE x RTSP/1.0\r\nCSeq: 8\r\n\r\n"));
    REQUIRE(exchange.done);
    REQUIRE(exchange.response.has_value());
    CHECK_FALSE(exchange.response->ok());
    CHECK(exchange.response->statusCode == 400);
    CHECK(exchange.response->statusMessage == "BAD REQUEST");
}

TEST_CASE("a request with no endpoint fails immediately", "[moonlight][rtspclient]") {
    MoonlightRtspClient client;
    CHECK_FALSE(client.isOpen());
    bool called = false;
    std::optional<moonrtsp::Response> got;
    client.request(QStringLiteral("OPTIONS * RTSP/1.0\r\nCSeq: 1\r\n\r\n"),
                   [&](const std::optional<moonrtsp::Response>& response) {
                       called = true;
                       got = response;
                   });
    CHECK(called);
    CHECK_FALSE(got.has_value());
}

TEST_CASE("close() cancels an in-flight request", "[moonlight][rtspclient]") {
    // A host that accepts and then says nothing, so the request is still open
    // when the session tears down.
    FixtureHost host([](QTcpSocket*, const QByteArray&) {});
    REQUIRE(host.listening());

    MoonlightRtspClient client;
    client.open(QStringLiteral("127.0.0.1"), host.port());
    auto exchange = std::make_shared<Exchange>();
    client.request(QStringLiteral("OPTIONS * RTSP/1.0\r\nCSeq: 1\r\n\r\n"),
                   [exchange](const std::optional<moonrtsp::Response>& response) {
                       exchange->response = response;
                       exchange->done = true;
                   });
    spinUntil([&host] { return host.connections() > 0; }, 2000);
    client.close();
    CHECK(exchange->done);
    CHECK_FALSE(exchange->response.has_value());
    CHECK_FALSE(client.isOpen());

    // And a request after close is refused rather than dialling again.
    const int before = host.connections();
    bool called = false;
    client.request(QStringLiteral("OPTIONS * RTSP/1.0\r\nCSeq: 2\r\n\r\n"),
                   [&called](const std::optional<moonrtsp::Response>&) { called = true; });
    CHECK(called);
    CHECK(host.connections() == before);
}

TEST_CASE("a second request supersedes the one still in flight", "[moonlight][rtspclient]") {
    FixtureHost host([](QTcpSocket* sock, const QByteArray& request) {
        // Only the second request is ever answered.
        if (request.contains("CSeq: 2")) {
            sock->write("RTSP/1.0 200 OK\r\nCSeq: 2\r\n\r\n");
            sock->flush();
            sock->disconnectFromHost();
        }
    });
    REQUIRE(host.listening());

    MoonlightRtspClient client;
    client.open(QStringLiteral("127.0.0.1"), host.port());
    auto first = std::make_shared<Exchange>();
    client.request(QStringLiteral("OPTIONS * RTSP/1.0\r\nCSeq: 1\r\n\r\n"),
                   [first](const std::optional<moonrtsp::Response>& response) {
                       first->response = response;
                       first->done = true;
                   });
    spinUntil([&host] { return host.connections() > 0; }, 2000);

    const auto second = ask(client, QStringLiteral("OPTIONS * RTSP/1.0\r\nCSeq: 2\r\n\r\n"));
    CHECK(first->done);
    CHECK_FALSE(first->response.has_value());
    REQUIRE(second.done);
    REQUIRE(second.response.has_value());
    CHECK(second.response->cseq == 2);
    CHECK(host.connections() == 2);
}
