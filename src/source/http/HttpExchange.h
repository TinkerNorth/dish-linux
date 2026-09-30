// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// One HTTP/1.1 request and its response over a socket that is dialled, handshaken, written, read
// and closed on the thread that asked for it. QNetworkAccessManager runs TLS on a thread of its
// own and hands the peer certificate OpenSSL parsed there to the pin check here, ordered by a lock
// inside Qt that ThreadSanitizer cannot see; nothing here crosses a thread.
//
// The bytes are core/wire/HttpFraming's; this moves them and keeps the deadline.

#pragma once

#include "core/reducer/RestOutcome.h"
#include "core/wire/HttpFraming.h"

#include <QAbstractSocket>
#include <QByteArray>
#include <QSslConfiguration>
#include <QString>
#include <QUrl>

#include <functional>
#include <optional>
#include <vector>

class QObject;

namespace dish::http {

struct HttpRequest {
    // Host, port, path and query. The port is always spelled out.
    QUrl url;
    QByteArray method;
    std::vector<wire::HttpHeader> headers;
    QByteArray body;
    // Set, the exchange runs TLS under this configuration; unset, plain TCP.
    std::optional<QSslConfiguration> tls;
    int timeoutMs = 0;
};

struct HttpResult {
    // Status 0 when nothing answered, and `failure` then says why.
    wire::HttpResponse response;
    reducer::TransportFailure failure = reducer::TransportFailure::None;
    // The failure in words, for a log line.
    QString error;
};

// Shown the peer's certificate (DER) when the handshake completes, before a byte of the request is
// written; false hangs up. Without one, any certificate is accepted.
using PeerCheck = std::function<bool(const QByteArray& certDer)>;
using HttpDone = std::function<void(const HttpResult&)>;

// Sends `request` and hands its outcome to `done` exactly once, from the event loop and never from
// inside this call. Destroying `owner` first cancels the exchange without calling `done`.
void exchangeHttp(QObject* owner, HttpRequest request, PeerCheck peerCheck, HttpDone done);

// The transport failure a socket error stands for, exposed so the mapping can be tested.
reducer::TransportFailure transportFailureOf(QAbstractSocket::SocketError error);

} // namespace dish::http
