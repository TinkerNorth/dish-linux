// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The RTSP handshake transport: one QTcpSocket, one request in flight at a
// time, strict request->response alternation (which is all the protocol ever
// does). Formatting/parsing lives in core/moonlight/MoonlightRtsp; this class
// only moves bytes and applies a per-request timeout.

#pragma once

#include "core/moonlight/MoonlightRtsp.h"

#include <QByteArray>
#include <QObject>
#include <QString>

#include <functional>
#include <optional>

class QTcpSocket;
class QTimer;

namespace dish::source::moon {

class MoonlightRtspClient : public QObject {
    Q_OBJECT
  public:
    explicit MoonlightRtspClient(QObject* parent = nullptr);
    ~MoonlightRtspClient() override;

    // nullopt = timeout, transport drop, or an unparsable response.
    using ResponseCb = std::function<void(const std::optional<moonrtsp::Response>&)>;

    // Dials `address:port` (the REAL host address; the launch response's
    // rtsp target string is only ever parroted inside requests).
    void open(const QString& address, int port);
    void close();
    bool isOpen() const;

    // Sends one formatted request and delivers its response. A request while
    // another is pending fails the pending one first.
    void request(const QString& text, ResponseCb cb);

  signals:
    void connected();
    void transportError();

  private:
    void onReadyRead();
    void finish(const std::optional<moonrtsp::Response>& response);

    QTcpSocket* socket_;
    QTimer* timeout_;
    QByteArray buffer_;
    ResponseCb pending_;

    static constexpr int kRequestTimeoutMs = 5000;
    static constexpr int kConnectTimeoutMs = 5000;
};

} // namespace dish::source::moon
