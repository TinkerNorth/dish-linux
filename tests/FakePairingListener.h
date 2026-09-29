// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// A satellite's HTTPS endpoint on loopback, reduced to what a client can
// observe: it records every request and answers each one through a responder
// the test sets. TLS is real and uses the Moonlight fixture's self-signed
// identity, so the client's pinned-certificate check runs against a real
// handshake.
//
// It lives on the test's own thread, beside the client under test; the test
// spins that thread (dish::test::spinFor) while it waits for a reply.

#pragma once

#include "FixtureIdentity.h"
#include "TestEventLoop.h"

#include <QByteArray>
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
#include <QUrl>
#include <QUrlQuery>

#include <functional>
#include <memory>
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
    // transfer timeout, so a release has to come well inside it.
    void hold() { holding_ = true; }
    void release() {
        holding_ = false;
        for (const auto& [sock, answer] : held_) {
            if (sock) { write(sock, answer); }
        }
        held_.clear();
    }

  private:
    void acceptAll() {
        while (auto* sock = server_.nextPendingConnection()) {
            auto pending = std::make_shared<QByteArray>();
            QObject::connect(sock, &QTcpSocket::readyRead, sock,
                             [this, sock, pending] { onBytes(sock, *pending); });
            QObject::connect(sock, &QTcpSocket::disconnected, sock, &QObject::deleteLater);
        }
    }

    // Answers once the whole request, headers and Content-Length body, has arrived.
    void onBytes(QTcpSocket* sock, QByteArray& pending) {
        pending.append(sock->readAll());
        const qsizetype headerEnd = pending.indexOf("\r\n\r\n");
        if (headerEnd < 0) { return; }
        const QByteArray head = pending.left(headerEnd);
        const qsizetype bodyStart = headerEnd + 4;
        const qsizetype bodyLength = contentLength(head);
        if (pending.size() - bodyStart < bodyLength) { return; }

        const QList<QByteArray> requestLine = head.split('\n').first().trimmed().split(' ');
        const QUrl target(QString::fromLatin1(requestLine.value(1)));
        SeenRequest seenRequest;
        seenRequest.method = requestLine.value(0);
        seenRequest.path = target.path();
        seenRequest.query = QUrlQuery(target);
        seenRequest.body = QJsonDocument::fromJson(pending.mid(bodyStart, bodyLength)).object();
        requests_.push_back(seenRequest);

        const PairingAnswer answer = respond(seenRequest);
        if (holding_) {
            held_.emplace_back(QPointer<QTcpSocket>(sock), answer);
            return;
        }
        write(sock, answer);
    }

    static void write(QTcpSocket* sock, const PairingAnswer& answer) {
        const QByteArray payload = QJsonDocument(answer.body).toJson(QJsonDocument::Compact);
        sock->write("HTTP/1.1 " + QByteArray::number(answer.status) + " X\r\n" +
                    "Content-Type: application/json\r\n" +
                    "Content-Length: " + QByteArray::number(payload.size()) + "\r\n" +
                    "Connection: close\r\n\r\n" + payload);
        sock->disconnectFromHost();
    }

    static qsizetype contentLength(const QByteArray& head) {
        for (const QByteArray& line : head.split('\n')) {
            const QByteArray trimmed = line.trimmed();
            if (trimmed.toLower().startsWith("content-length:")) {
                return trimmed.mid(15).trimmed().toLongLong();
            }
        }
        return 0;
    }

    QSslServer server_;
    QSslCertificate cert_;
    bool listening_ = false;
    bool holding_ = false;
    std::vector<SeenRequest> requests_;
    std::vector<std::pair<QPointer<QTcpSocket>, PairingAnswer>> held_;
};

} // namespace dish::test
