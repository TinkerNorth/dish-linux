// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "PairingClient.h"

#include <QJsonDocument>
#include <QJsonObject>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace dish::net {

namespace {

models::PairResponse makeError(const char* msg) {
    models::PairResponse r;
    r.ok = false;
    r.error = QString::fromLatin1(msg);
    // Synthesized network-error responses are unreachable by construction —
    // we never made it far enough to receive a JSON body. fromJson flips this
    // to true on the success path.
    r.reachable = false;
    return r;
}

} // namespace

PairingClient::Outcome PairingClient::classify(const models::PairResponse& response) {
    if (response.ok && response.sharedKey.has_value() && !response.sharedKey->isEmpty()) {
        return Success{*response.sharedKey};
    }
    if (response.reachable) { return AuthRequired{}; }
    return Unreachable{response.error.value_or(QStringLiteral("Server unreachable"))};
}

models::PairResponse PairingClient::pair(const QString& ip, int port, const QString& deviceId,
                                         const QString& deviceName, const QString& pin) {
    const int sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) { return makeError("socket failed"); }

    const int flags = ::fcntl(sock, F_GETFL, 0);
    ::fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(port));
    if (::inet_pton(AF_INET, ip.toUtf8().constData(), &addr.sin_addr) != 1) {
        ::close(sock);
        return makeError("bad ip");
    }

    const int connectRet = ::connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (connectRet != 0 && errno != EINPROGRESS) {
        ::close(sock);
        return makeError("connect failed");
    }
    if (connectRet != 0) {
        timeval tv{};
        tv.tv_sec = 4;
        fd_set wset;
        FD_ZERO(&wset);
        FD_SET(sock, &wset);
        const int sel = ::select(sock + 1, nullptr, &wset, nullptr, &tv);
        if (sel <= 0) {
            ::close(sock);
            return makeError("connect timeout");
        }
        int sockerr = 0;
        socklen_t sl = sizeof(sockerr);
        ::getsockopt(sock, SOL_SOCKET, SO_ERROR, &sockerr, &sl);
        if (sockerr != 0) {
            ::close(sock);
            return makeError("connect refused");
        }
    }
    ::fcntl(sock, F_SETFL, flags & ~O_NONBLOCK);

    const QJsonObject reqObj{{"deviceId", deviceId}, {"deviceName", deviceName}, {"pin", pin}};
    const auto body = QJsonDocument(reqObj).toJson(QJsonDocument::Compact);
    if (::send(sock, body.constData(), static_cast<std::size_t>(body.size()), MSG_NOSIGNAL) < 0) {
        ::close(sock);
        return makeError("send failed");
    }

    timeval rtv{};
    rtv.tv_sec = 5;
    ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &rtv, sizeof(rtv));

    char buf[512];
    const ssize_t n = ::recv(sock, buf, sizeof(buf), 0);
    ::close(sock);
    if (n <= 0) { return makeError("no response"); }

    QJsonParseError err{};
    const auto doc = QJsonDocument::fromJson(QByteArray(buf, static_cast<int>(n)), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        return makeError("malformed response");
    }
    return models::PairResponse::fromJson(doc.object());
}

} // namespace dish::net
