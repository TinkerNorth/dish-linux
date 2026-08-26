// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "source/moonlight/MoonlightRtspClient.h"

#include "source/moonlight/MoonlightLog.h"

#include <QMetaObject>
#include <QTcpSocket>
#include <QTimer>

#include <utility>

namespace dish::source::moon {
namespace {

// Line ends spelled out, so a framing bug is readable in a log line.
QString escaped(const QByteArray& raw) {
    QString out = QString::fromLatin1(raw.left(512));
    out.replace(QLatin1String("\r"), QLatin1String("\\r"));
    out.replace(QLatin1String("\n"), QLatin1String("\\n"));
    return out;
}

// Offset just past the blank line that ends the head, or -1 while it is short.
int headEnd(const QByteArray& buffer) {
    const int crlf = static_cast<int>(buffer.indexOf("\r\n\r\n"));
    if (crlf >= 0) { return crlf + 4; }
    const int lf = static_cast<int>(buffer.indexOf("\n\n"));
    if (lf >= 0) { return lf + 2; }
    return -1;
}

} // namespace

MoonlightRtspClient::MoonlightRtspClient(QObject* parent)
    : QObject(parent), timeout_(new QTimer(this)) {
    timeout_->setSingleShot(true);
    QObject::connect(timeout_, &QTimer::timeout, this, [this] {
        qCWarning(lcMoon) << "rtsp" << stage_ << "timed out after" << kRequestTimeoutMs << "ms";
        finish(std::nullopt);
    });
}

MoonlightRtspClient::~MoonlightRtspClient() { dropSocket(); }

void MoonlightRtspClient::open(const QString& address, int port) {
    close();
    address_ = address;
    port_ = port;
    open_ = true;
    const unsigned generation = generation_;
    qCDebug(lcMoon) << "rtsp endpoint" << address_ << port_;
    // Nothing to dial: every request brings its own socket. The ready notice
    // is queued so the caller's effect loop is not re-entered.
    QMetaObject::invokeMethod(
        this,
        [this, generation] {
            if (open_ && generation == generation_) { emit connected(); }
        },
        Qt::QueuedConnection);
}

void MoonlightRtspClient::close() {
    ++generation_;
    open_ = false;
    finish(std::nullopt);
    dropSocket();
    buffer_.clear();
    outgoing_.clear();
}

bool MoonlightRtspClient::isOpen() const { return open_; }

void MoonlightRtspClient::dropSocket() {
    if (socket_ == nullptr) { return; }
    QTcpSocket* socket = socket_;
    socket_ = nullptr;
    socket->disconnect(this);
    socket->abort(); // no graceful RTSP TEARDOWN exists in this dialect
    socket->deleteLater();
}

void MoonlightRtspClient::request(const QString& text, ResponseCb cb) {
    finish(std::nullopt); // supersede any stalled request
    dropSocket();
    buffer_.clear();
    if (!open_) {
        qCWarning(lcMoon) << "rtsp request with no endpoint";
        if (cb) { cb(std::nullopt); }
        return;
    }

    stage_ = text.section(QLatin1Char('\n'), 0, 0).trimmed();
    outgoing_ = text.toUtf8();
    pending_ = std::move(cb);

    socket_ = new QTcpSocket(this);
    QObject::connect(socket_, &QTcpSocket::connected, this, &MoonlightRtspClient::onConnected);
    QObject::connect(socket_, &QTcpSocket::readyRead, this, &MoonlightRtspClient::onReadyRead);
    QObject::connect(socket_, &QTcpSocket::disconnected, this,
                     &MoonlightRtspClient::onDisconnected);
    QObject::connect(
        socket_, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError error) {
            if (error == QAbstractSocket::RemoteHostClosedError) {
                return; // the hang-up that frames the reply
            }
            qCWarning(lcMoon) << "rtsp" << stage_ << "failed:"
                              << (socket_ != nullptr ? socket_->errorString() : QString());
            finish(std::nullopt);
            emit transportError();
        });
    timeout_->start(kRequestTimeoutMs);
    socket_->connectToHost(address_, static_cast<quint16>(port_));
}

void MoonlightRtspClient::onConnected() {
    qCDebug(lcMoon) << "rtsp ->" << stage_;
    if (socket_ != nullptr) { socket_->write(outgoing_); }
}

void MoonlightRtspClient::onReadyRead() {
    if (socket_ == nullptr) { return; }
    buffer_.append(socket_->readAll());
    tryComplete(false);
}

void MoonlightRtspClient::onDisconnected() {
    if (socket_ != nullptr) { buffer_.append(socket_->readAll()); }
    // The host hangs up once it has answered, so end-of-stream is a framing
    // signal and not by itself a failure.
    tryComplete(true);
}

void MoonlightRtspClient::tryComplete(bool atEof) {
    if (!pending_) { return; }
    const int bodyAt = headEnd(buffer_);
    if (bodyAt < 0) {
        if (!atEof) { return; }
        qCWarning(lcMoon) << "rtsp" << stage_
                          << "closed before a complete reply head:" << escaped(buffer_);
        finish(std::nullopt);
        return;
    }
    const auto head = moonrtsp::parseResponse(
        std::string_view(buffer_.constData(), static_cast<std::size_t>(bodyAt)));
    if (!head) {
        if (!atEof) { return; }
        qCWarning(lcMoon) << "rtsp" << stage_ << "unparsable reply:" << escaped(buffer_);
        finish(std::nullopt);
        return;
    }
    const auto declared = moonrtsp::contentLength(*head);
    if (declared) {
        const int have = static_cast<int>(buffer_.size()) - bodyAt;
        if (have < *declared && !atEof) { return; }
    } else if (!atEof) {
        // No Content-length: the DESCRIBE shape. The close is the frame, so
        // wait for it rather than truncating the payload.
        return;
    }
    finish(moonrtsp::parseResponse(
        std::string_view(buffer_.constData(), static_cast<std::size_t>(buffer_.size()))));
}

void MoonlightRtspClient::finish(const std::optional<moonrtsp::Response>& response) {
    timeout_->stop();
    if (!pending_) { return; }
    ResponseCb cb = std::move(pending_);
    pending_ = nullptr;
    if (response) {
        if (response->ok()) {
            qCDebug(lcMoon) << "rtsp <-" << stage_ << response->statusCode
                            << QString::fromStdString(response->statusMessage);
        } else {
            qCWarning(lcMoon) << "rtsp" << stage_ << "refused:" << response->statusCode
                              << QString::fromStdString(response->statusMessage);
        }
    }
    cb(response);
}

} // namespace dish::source::moon
