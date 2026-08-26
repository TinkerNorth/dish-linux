// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The RTSP handshake transport: ONE TCP CONNECTION PER MESSAGE, because that
// is all a Moonlight host will give. The host answers exactly one RTSP message
// and then hangs up on its own: an idle read taken straight after the OPTIONS
// reply returns end-of-stream, and so does the same read after a DESCRIBE
// reply, so it is having answered that ends the connection and not which
// command was asked. A second message written into that socket is never seen at
// all. So every request dials its own socket, asks, reads the answer and
// closes, the same shape the HTTP half already has.
//
// That hang-up frames the body as much as Content-length does: the DESCRIBE
// reply carries no length header, so a reply without one is read to EOF.
//
// Formatting/parsing lives in core/moonlight/MoonlightRtsp; this class only
// moves bytes, frames replies and applies per-request timeouts.

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

    // Records the endpoint every later request dials — the REAL host address;
    // the launch response's rtsp target string is only ever parroted inside
    // requests. Nothing is dialled here, so `connected` is reported on the next
    // event-loop turn and the first real reachability answer arrives with the
    // first request.
    void open(const QString& address, int port);
    void close();
    bool isOpen() const;

    // Sends one formatted request over its own socket and delivers its
    // response. A request while another is pending fails the pending one first.
    void request(const QString& text, ResponseCb cb);

    // The step in flight, as it would be named in a log line.
    const QString& stage() const { return stage_; }

  signals:
    void connected();
    void transportError();

  private:
    void onConnected();
    void onReadyRead();
    void onDisconnected();
    void tryComplete(bool atEof);
    void finish(const std::optional<moonrtsp::Response>& response);
    void dropSocket();

    QString address_;
    int port_ = 0;
    bool open_ = false;
    // Bumped by every open()/close() so a queued ready notice from a superseded
    // endpoint cannot reach the caller.
    unsigned generation_ = 0;

    QTcpSocket* socket_ = nullptr;
    QTimer* timeout_;
    QByteArray buffer_;
    QByteArray outgoing_;
    ResponseCb pending_;
    QString stage_;

    static constexpr int kRequestTimeoutMs = 5000;
};

} // namespace dish::source::moon
