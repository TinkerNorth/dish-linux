// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// A satellite's HTTPS endpoint on loopback, reduced to what a client can
// observe: it records every request, headers and all, and answers each one
// through a responder the test sets, or with exactly the bytes a test wants on
// the wire. TLS is real and uses the Moonlight fixture's self-signed identity,
// so the client's pinned-certificate check runs against a real handshake.
//
// It lives on the test's own thread, beside the client under test; the test
// spins that thread (dish::test::spinFor) while it waits for a reply.

#pragma once

#include "FixtureIdentity.h"
#include "TestEventLoop.h"

#include <QByteArray>
#include <QHash>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QPointer>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslKey>
#include <QSslServer>
#include <QSslSocket>
#include <QString>
#include <QTcpServer>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace dish::test {

// A loopback port with nothing listening on it.
inline int closedLoopbackPort() {
    QTcpServer probe;
    if (!probe.listen(QHostAddress::LocalHost, 0)) { return 0; }
    const int port = static_cast<int>(probe.serverPort());
    probe.close();
    return port;
}

// One request as the listener received it.
struct SeenRequest {
    QByteArray method;
    QString path;
    QUrlQuery query;
    QJsonObject body;
    // Every header field, keyed by its name in lower case.
    QHash<QByteArray, QByteArray> headers;
    // What the client presented, once the listener asks for one; null until then.
    QSslCertificate clientCertificate;
};

// What the listener says back.
struct PairingAnswer {
    int status = 200;
    QJsonObject body;
};

class FakePairingListener : public QObject {
  public:
    // Every request is answered by `respond`; by default a 200 carrying a key,
    // which is a pair that succeeded outright.
    std::function<PairingAnswer(const SeenRequest&)> respond = [](const SeenRequest&) {
        return PairingAnswer{
            200, QJsonObject{{QStringLiteral("ok"), true},
                             {QStringLiteral("sharedKey"), QStringLiteral("00112233")}}};
    };

    // When set, every request is answered with exactly these bytes instead: how a test puts
    // chunked framing, an ETag or a broken answer on the wire.
    std::function<QByteArray(const SeenRequest&)> respondRaw;

    // When positive, every answer goes out as two writes a moment apart, cut after this many
    // bytes: an answer whose head arrives in pieces.
    int splitAt = 0;

    // On IPv4 loopback unless a test needs the other one.
    explicit FakePairingListener(const QHostAddress& on = QHostAddress(QHostAddress::LocalHost)) {
        const auto& identity = fixtureHostIdentity();
        const auto certs =
            QSslCertificate::fromData(QByteArray::fromStdString(identity.certPem), QSsl::Pem);
        if (certs.isEmpty()) { return; }
        cert_ = certs.first();
        QSslConfiguration ssl = QSslConfiguration::defaultConfiguration();
        ssl.setLocalCertificate(cert_);
        ssl.setPrivateKey(QSslKey(QByteArray::fromStdString(identity.privateKeyPem), QSsl::Rsa,
                                  QSsl::Pem, QSsl::PrivateKey));
        ssl.setPeerVerifyMode(QSslSocket::VerifyNone);
        server_.setSslConfiguration(ssl);
        QObject::connect(&server_, &QTcpServer::pendingConnectionAvailable, this,
                         &FakePairingListener::acceptAll);
        listening_ = server_.listen(on, 0);
    }

    bool listening() const { return listening_; }
    int port() const { return static_cast<int>(server_.serverPort()); }
    QByteArray certDer() const { return cert_.toDer(); }

    const std::vector<SeenRequest>& requests() const { return requests_; }

    // Every byte any connection delivered, whether or not it made a whole request: what "nothing
    // was written" is measured by.
    qsizetype bytesReceived() const { return bytesReceived_; }

    // From now on the handshake asks the client for its certificate, and each request records the
    // one it presented.
    void askForClientCertificates() {
        QSslConfiguration ssl = server_.sslConfiguration();
        ssl.setPeerVerifyMode(QSslSocket::QueryPeer);
        server_.setSslConfiguration(ssl);
    }

    // How many requests reached a path, whatever they carried.
    int seen(const QString& path) const {
        int n = 0;
        for (const auto& r : requests_) {
            if (r.path == path) { ++n; }
        }
        return n;
    }

    // From hold() until release(), requests are recorded but their answers wait: the window a
    // test needs to act while a reply is still on its way. The client gives up after its own
    // timeout, so a release has to come well inside it.
    void hold() { holding_ = true; }
    void release() {
        holding_ = false;
        for (const auto& [sock, answer] : held_) {
            if (sock) { write(sock, answer); }
        }
        held_.clear();
    }

  private:
    // Long enough that the client has read the first part on its own before the rest exists.
    static constexpr int kSplitPauseMs = 100;

    void acceptAll() {
        while (auto* sock = qobject_cast<QSslSocket*>(server_.nextPendingConnection())) {
            auto pending = std::make_shared<QByteArray>();
            QObject::connect(sock, &QSslSocket::readyRead, sock,
                             [this, sock, pending] { onBytes(sock, *pending); });
            QObject::connect(sock, &QSslSocket::disconnected, sock, &QObject::deleteLater);
        }
    }

    // Answers once the whole request, headers and Content-Length body, has arrived.
    void onBytes(QSslSocket* sock, QByteArray& pending) {
        const QByteArray fresh = sock->readAll();
        bytesReceived_ += fresh.size();
        pending.append(fresh);
        std::optional<SeenRequest> seenRequest = wholeRequest(pending);
        if (!seenRequest) { return; }
        seenRequest->clientCertificate = sock->peerCertificate();
        requests_.push_back(*seenRequest);

        const QByteArray answer =
            respondRaw ? respondRaw(*seenRequest) : encoded(respond(*seenRequest));
        if (holding_) {
            held_.emplace_back(QPointer<QSslSocket>(sock), answer);
            return;
        }
        write(sock, answer);
    }

    // The request `pending` holds, once its head and Content-Length body have both arrived.
    static std::optional<SeenRequest> wholeRequest(const QByteArray& pending) {
        const qsizetype headerEnd = pending.indexOf("\r\n\r\n");
        if (headerEnd < 0) { return std::nullopt; }
        const QByteArray head = pending.left(headerEnd);
        const QHash<QByteArray, QByteArray> headers = headersOf(head);
        const qsizetype bodyStart = headerEnd + 4;
        const qsizetype bodyLength = headers.value("content-length").toLongLong();
        if (pending.size() - bodyStart < bodyLength) { return std::nullopt; }

        const QList<QByteArray> requestLine = head.split('\n').first().trimmed().split(' ');
        const QUrl target(QString::fromLatin1(requestLine.value(1)));
        SeenRequest seenRequest;
        seenRequest.method = requestLine.value(0);
        seenRequest.path = target.path();
        seenRequest.query = QUrlQuery(target);
        seenRequest.body = QJsonDocument::fromJson(pending.mid(bodyStart, bodyLength)).object();
        seenRequest.headers = headers;
        return seenRequest;
    }

    // Every field line after the request line, keyed by its name in lower case.
    static QHash<QByteArray, QByteArray> headersOf(const QByteArray& head) {
        QHash<QByteArray, QByteArray> headers;
        const QList<QByteArray> lines = head.split('\n');
        for (qsizetype i = 1; i < lines.size(); ++i) {
            const QByteArray line = lines.at(i).trimmed();
            const qsizetype colon = line.indexOf(':');
            headers.insert(line.left(colon).trimmed().toLower(), line.mid(colon + 1).trimmed());
        }
        return headers;
    }

    // An answer framed the way the satellite frames one: JSON, its length, and a hang-up.
    static QByteArray encoded(const PairingAnswer& answer) {
        const QByteArray payload = QJsonDocument(answer.body).toJson(QJsonDocument::Compact);
        return "HTTP/1.1 " + QByteArray::number(answer.status) + " X\r\n" +
               "Content-Type: application/json\r\n" +
               "Content-Length: " + QByteArray::number(payload.size()) + "\r\n" +
               "Connection: close\r\n\r\n" + payload;
    }

    void write(QSslSocket* sock, const QByteArray& answer) {
        const bool inTwoWrites = splitAt > 0 && splitAt < answer.size();
        if (!inTwoWrites) {
            writeAndClose(sock, answer);
            return;
        }
        sock->write(answer.left(splitAt));
        sock->flush();
        const QByteArray rest = answer.mid(splitAt);
        QTimer::singleShot(kSplitPauseMs, sock, [sock, rest] { writeAndClose(sock, rest); });
    }

    static void writeAndClose(QSslSocket* sock, const QByteArray& bytes) {
        sock->write(bytes);
        sock->disconnectFromHost();
    }

    QSslServer server_;
    QSslCertificate cert_;
    bool listening_ = false;
    bool holding_ = false;
    qsizetype bytesReceived_ = 0;
    std::vector<SeenRequest> requests_;
    std::vector<std::pair<QPointer<QSslSocket>, QByteArray>> held_;
};

} // namespace dish::test
