// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// What a controller database request carries (docs/controller-db.md section
// 6.8, PRIVACY.md section 2.5): the request the gateway builds, sent by a real
// QNetworkAccessManager to a listener on loopback that records the bytes, so
// the headers Qt adds on its own are seen too.

#include "source/controllerdb/ControllerDbGateway.h"

#include "TestEventLoop.h"

#include <catch2/catch_test_macros.hpp>

#include <QByteArrayList>
#include <QHostAddress>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrl>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

using dish::source::controllerdb::controllerDbRequest;
using dish::source::controllerdb::kDbAcceptLanguage;

namespace {

// Keeps the first request's head and answers it with an empty 200.
class RecordingServer {
  public:
    RecordingServer() {
        listening_ = server_.listen(QHostAddress::LocalHost, 0);
        QObject::connect(&server_, &QTcpServer::newConnection, &server_,
                         [this] { onConnection(); });
    }

    bool listening() const { return listening_; }
    bool answered() const { return answered_; }
    QUrl url() const {
        return QUrl(QStringLiteral("http://127.0.0.1:%1/controllers/v1/manifest.json")
                        .arg(server_.serverPort()));
    }

    // The request's header lines, without the request line.
    QByteArrayList headerLines() const {
        QByteArrayList lines = head_.split('\n');
        QByteArrayList out;
        for (qsizetype i = 1; i < lines.size(); ++i) {
            const QByteArray line = lines[i].trimmed();
            if (!line.isEmpty()) { out.append(line); }
        }
        return out;
    }

    // Text rather than bytes, so a failure prints the lines readably.
    std::vector<std::string> linesNamed(const QByteArray& name) const {
        std::vector<std::string> out;
        for (const QByteArray& line : headerLines()) {
            if (line.toLower().startsWith(name.toLower() + ':')) {
                out.push_back(line.toStdString());
            }
        }
        return out;
    }

  private:
    void onConnection() {
        socket_ = server_.nextPendingConnection();
        QObject::connect(socket_, &QTcpSocket::readyRead, socket_, [this] { onReadyRead(); });
    }

    void onReadyRead() {
        head_.append(socket_->readAll());
        if (answered_ || !head_.contains("\r\n\r\n")) { return; }
        answered_ = true;
        socket_->write("HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        socket_->disconnectFromHost();
    }

    QTcpServer server_;
    QTcpSocket* socket_ = nullptr;
    QByteArray head_;
    bool listening_ = false;
    bool answered_ = false;
};

// Sends the request and waits for the listener to have it.
void sendThrough(RecordingServer& server, const QNetworkRequest& request) {
    QNetworkAccessManager nam;
    nam.setCookieJar(nullptr);
    std::unique_ptr<QNetworkReply> reply(nam.get(request));
    REQUIRE(dish::test::spinFor([&server] { return server.answered(); }));
    REQUIRE(dish::test::spinFor([&reply] { return reply->isFinished(); }));
}

} // namespace

TEST_CASE("controller db request: Accept-Language is fixed, never the system's languages",
          "[controllerdb][gateway]") {
    RecordingServer server;
    REQUIRE(server.listening());
    sendThrough(server, controllerDbRequest(server.url(), QByteArray()));
    CHECK(server.linesNamed("Accept-Language") ==
          std::vector<std::string>{std::string("Accept-Language: ") + kDbAcceptLanguage});
}

TEST_CASE("controller db request: If-None-Match only when there is an ETag",
          "[controllerdb][gateway]") {
    SECTION("with one") {
        RecordingServer server;
        REQUIRE(server.listening());
        sendThrough(server, controllerDbRequest(server.url(), QByteArrayLiteral("\"v1\"")));
        CHECK(server.linesNamed("If-None-Match") ==
              std::vector<std::string>{"If-None-Match: \"v1\""});
    }
    SECTION("without") {
        RecordingServer server;
        REQUIRE(server.listening());
        sendThrough(server, controllerDbRequest(server.url(), QByteArray()));
        CHECK(server.linesNamed("If-None-Match").empty());
    }
}

// Pins every header the request carries, so a new one cannot arrive unnoticed: the policy names
// each of them (PRIVACY.md section 2.5).
TEST_CASE("controller db request: it carries these headers and no others",
          "[controllerdb][gateway]") {
    RecordingServer server;
    REQUIRE(server.listening());
    sendThrough(server, controllerDbRequest(server.url(), QByteArray()));
    std::vector<std::string> names;
    for (const QByteArray& line : server.headerLines()) {
        names.push_back(line.left(line.indexOf(':')).toLower().toStdString());
    }
    std::sort(names.begin(), names.end());
    // Cache-Control and Pragma are Qt's for a request that must not be answered from a cache.
    CHECK(names == std::vector<std::string>{"accept-encoding", "accept-language", "cache-control",
                                            "connection", "host", "pragma", "user-agent"});
    CHECK(server.linesNamed("User-Agent") == std::vector<std::string>{"User-Agent: Mozilla/5.0"});
}
