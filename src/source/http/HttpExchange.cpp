// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "source/http/HttpExchange.h"

#include <QMetaObject>
#include <QObject>
#include <QSslCertificate>
#include <QSslSocket>
#include <QTimer>

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace dish::http {
namespace {

std::string_view viewOf(const QByteArray& bytes) {
    return {bytes.constData(), static_cast<std::size_t>(bytes.size())};
}

// The origin form, "/path?query", encoded the way QNetworkAccessManager encoded it.
std::string targetOf(const QUrl& url) {
    const QUrl::FormattingOptions originForm = QUrl::RemoveScheme | QUrl::RemoveAuthority |
                                               QUrl::RemoveFragment | QUrl::NormalizePathSegments;
    return url.toEncoded(originForm).toStdString();
}

quint16 portOf(const QUrl& url) { return static_cast<quint16>(url.port()); }

class Exchange : public QObject {
  public:
    Exchange(HttpRequest request, PeerCheck peerCheck, HttpDone done, QObject* owner);

    void start();

  private:
    void dialPlain();
    void dialEncrypted(const QSslConfiguration& tls);
    void onEncrypted();
    void send();
    void onReadyRead();
    void onDisconnected();
    void onError(QAbstractSocket::SocketError error);
    void onDeadline();
    void readResponse(bool peerClosed);
    void fail(reducer::TransportFailure failure, const QString& error);
    void finish(HttpResult result);
    void deliver();

    HttpRequest request_;
    PeerCheck peerCheck_;
    HttpDone done_;
    QSslSocket* socket_;
    QTimer* deadline_;
    QByteArray received_;
    HttpResult result_;
};

Exchange::Exchange(HttpRequest request, PeerCheck peerCheck, HttpDone done, QObject* owner)
    : QObject(owner), request_(std::move(request)), peerCheck_(std::move(peerCheck)),
      done_(std::move(done)), socket_(new QSslSocket(this)), deadline_(new QTimer(this)) {
    deadline_->setSingleShot(true);
    QObject::connect(deadline_, &QTimer::timeout, this, &Exchange::onDeadline);
    QObject::connect(socket_, &QSslSocket::readyRead, this, &Exchange::onReadyRead);
    QObject::connect(socket_, &QSslSocket::disconnected, this, &Exchange::onDisconnected);
    QObject::connect(socket_, &QSslSocket::errorOccurred, this, &Exchange::onError);
}

void Exchange::start() {
    deadline_->start(request_.timeoutMs);
    if (request_.tls.has_value()) {
        dialEncrypted(*request_.tls);
        return;
    }
    dialPlain();
}

void Exchange::dialPlain() {
    QObject::connect(socket_, &QSslSocket::connected, this, &Exchange::send);
    socket_->connectToHost(request_.url.host(), portOf(request_.url));
}

// Every caller's configuration turns peer verification off, the certificates being self-signed,
// so no certificate error stops the handshake; trust is the pin onEncrypted checks.
void Exchange::dialEncrypted(const QSslConfiguration& tls) {
    QObject::connect(socket_, &QSslSocket::encrypted, this, &Exchange::onEncrypted);
    socket_->setSslConfiguration(tls);
    socket_->connectToHostEncrypted(request_.url.host(), portOf(request_.url));
}

// The first moment the peer's certificate can be checked, and still before a byte of the request,
// secrets included, has been written.
void Exchange::onEncrypted() {
    const bool trusted = !peerCheck_ || peerCheck_(socket_->peerCertificate().toDer());
    if (!trusted) {
        fail(reducer::TransportFailure::Aborted,
             QStringLiteral("the peer certificate was refused"));
        return;
    }
    send();
}

void Exchange::send() {
    const std::string bytes =
        wire::formatHttpRequest(viewOf(request_.method), targetOf(request_.url),
                                request_.url.authority(QUrl::FullyEncoded).toStdString(),
                                request_.headers, viewOf(request_.body));
    socket_->write(bytes.data(), static_cast<qint64>(bytes.size()));
}

void Exchange::onReadyRead() {
    received_.append(socket_->readAll());
    readResponse(false);
}

void Exchange::onDisconnected() {
    received_.append(socket_->readAll());
    readResponse(true);
}

// A peer that hangs up is judged by what it sent before it did, in onDisconnected: that is how a
// body with no declared length ends.
void Exchange::onError(QAbstractSocket::SocketError error) {
    if (error == QAbstractSocket::RemoteHostClosedError) { return; }
    fail(transportFailureOf(error), socket_->errorString());
}

void Exchange::onDeadline() {
    fail(reducer::TransportFailure::TimedOut,
         QStringLiteral("no answer within %1 ms").arg(request_.timeoutMs));
}

void Exchange::readResponse(bool peerClosed) {
    wire::HttpParse parse = wire::parseHttpResponse(viewOf(received_), peerClosed);
    switch (parse.state) {
    case wire::HttpParse::State::Incomplete:
        return;
    case wire::HttpParse::State::Malformed:
        fail(reducer::TransportFailure::Other,
             QStringLiteral("the answer was not a whole HTTP response"));
        return;
    case wire::HttpParse::State::Complete:
        finish(HttpResult{std::move(parse.response), reducer::TransportFailure::None, QString()});
        return;
    }
}

void Exchange::fail(reducer::TransportFailure failure, const QString& error) {
    finish(HttpResult{wire::HttpResponse{}, failure, error});
}

// The socket stops talking to this the moment the outcome is settled, so nothing can arrive after
// it; the socket is closed, and the outcome handed over, from the event loop rather than from
// inside one of the socket's own signals.
void Exchange::finish(HttpResult result) {
    deadline_->stop();
    socket_->disconnect(this);
    result_ = std::move(result);
    QMetaObject::invokeMethod(this, &Exchange::deliver, Qt::QueuedConnection);
}

// `done` goes last and nothing of this is touched after it: it may destroy the owner, and this
// with it.
void Exchange::deliver() {
    socket_->abort();
    const HttpDone done = std::move(done_);
    const HttpResult result = std::move(result_);
    deleteLater();
    done(result);
}

} // namespace

void exchangeHttp(QObject* owner, HttpRequest request, PeerCheck peerCheck, HttpDone done) {
    auto* exchange = new Exchange(std::move(request), std::move(peerCheck), std::move(done), owner);
    exchange->start();
}

reducer::TransportFailure transportFailureOf(QAbstractSocket::SocketError error) {
    switch (error) {
    case QAbstractSocket::ConnectionRefusedError:
        return reducer::TransportFailure::Refused;
    case QAbstractSocket::SslHandshakeFailedError:
        return reducer::TransportFailure::Tls;
    default:
        // Host not found, no route, and the other ways a dial comes to nothing.
        return reducer::TransportFailure::Unreachable;
    }
}

} // namespace dish::http
