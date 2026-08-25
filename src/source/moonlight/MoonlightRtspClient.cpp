// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "source/moonlight/MoonlightRtspClient.h"

#include <QTcpSocket>
#include <QTimer>

#include <utility>

namespace dish::source::moon {

MoonlightRtspClient::MoonlightRtspClient(QObject* parent)
    : QObject(parent), socket_(new QTcpSocket(this)), timeout_(new QTimer(this)) {
    timeout_->setSingleShot(true);
    QObject::connect(timeout_, &QTimer::timeout, this, [this] { finish(std::nullopt); });
    QObject::connect(socket_, &QTcpSocket::connected, this, &MoonlightRtspClient::connected);
    QObject::connect(socket_, &QTcpSocket::readyRead, this, &MoonlightRtspClient::onReadyRead);
    QObject::connect(socket_, &QTcpSocket::errorOccurred, this,
                     [this](QAbstractSocket::SocketError) {
                         finish(std::nullopt);
                         emit transportError();
                     });
}

MoonlightRtspClient::~MoonlightRtspClient() = default;

void MoonlightRtspClient::open(const QString& address, int port) {
    close();
    socket_->connectToHost(address, static_cast<quint16>(port));
    // A silent connect failure surfaces through errorOccurred; Qt applies its
    // own connect timeout, so no extra timer is armed here.
}

void MoonlightRtspClient::close() {
    finish(std::nullopt);
    if (socket_->state() != QAbstractSocket::UnconnectedState) {
        socket_->abort(); // no graceful RTSP TEARDOWN exists in this dialect
    }
    buffer_.clear();
}

bool MoonlightRtspClient::isOpen() const {
    return socket_->state() == QAbstractSocket::ConnectedState;
}

void MoonlightRtspClient::request(const QString& text, ResponseCb cb) {
    finish(std::nullopt); // supersede any stalled request
    if (!isOpen()) {
        if (cb) { cb(std::nullopt); }
        return;
    }
    // Any bytes still buffered belong to the previous exchange (a DESCRIBE
    // payload tail, say) and must not pollute this one.
    buffer_.clear();
    pending_ = std::move(cb);
    timeout_->start(kRequestTimeoutMs);
    socket_->write(text.toUtf8());
}

void MoonlightRtspClient::onReadyRead() {
    buffer_.append(socket_->readAll());
    if (!pending_) { return; }
    // The status line and options are complete once the blank line lands; the
    // payload (only DESCRIBE has one) is informational and not waited for.
    if (!buffer_.contains("\r\n\r\n") && !buffer_.contains("\n\n")) { return; }
    const auto response = moonrtsp::parseResponse(
        std::string_view(buffer_.constData(), static_cast<std::size_t>(buffer_.size())));
    finish(response);
}

void MoonlightRtspClient::finish(const std::optional<moonrtsp::Response>& response) {
    timeout_->stop();
    if (!pending_) { return; }
    ResponseCb cb = std::move(pending_);
    pending_ = nullptr;
    cb(response);
}

} // namespace dish::source::moon
