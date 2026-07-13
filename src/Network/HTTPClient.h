// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#pragma once

#include "Models/Models.h"

#include <QByteArray>
#include <QObject>
#include <QString>

#include <functional>

class QNetworkAccessManager;

namespace dish::net {

// Async gateway to the satellite's protocol-1 client REST API (HTTPS :9443).
// Mirrors dish-windows/Network/HTTPClient and dish-android's
// SatelliteHttpClient. Every authenticated route attaches `X-Device-Id` +
// `X-Hmac-Proof` (the proof = hex(HMAC-SHA256(pairingKey,
// "satellite-proof:" + deviceId)), computed by the caller via
// Network/SessionCrypto and passed in). Callbacks fire on the network
// manager's home thread (the Qt main thread).
//
// TLS: the satellite presents a self-signed cert, so there is no CA chain to
// validate — peer verification stays VerifyNone (the equivalent of
// `curl --insecure`). Trust is instead enforced by TOFU cert-pinning: after
// the handshake completes, `pinVerifier_` (if installed) is handed the peer
// cert's DER bytes keyed by host; it pins on first contact and aborts the
// request if a later cert's fingerprint differs (anti-MITM). The verifier
// composes Network/Tofu + ConnectionStore's pin registry; when no verifier is
// installed behaviour is the legacy VerifyNone (nothing regresses).
class HTTPClient : public QObject {
    Q_OBJECT
  public:
    explicit HTTPClient(QObject* parent = nullptr);
    ~HTTPClient() override;

    // The TOFU pin seam: given the request host (ip) and the peer cert's DER
    // bytes, return true to trust (and let the request proceed) or false to
    // reject (the request is aborted). Installed by the composition root.
    using PinVerifier = std::function<bool(const QString& host, const QByteArray& certDer)>;
    void setPinVerifier(PinVerifier verifier) { pinVerifier_ = std::move(verifier); }

    using SessionCb = std::function<void(const models::SessionResponse&)>;
    using ControllerCb = std::function<void(const models::ControllerPutResponse&)>;
    using ViewCb = std::function<void(const models::SessionViewDto&)>;
    // Generic ack for routes the caller doesn't decode (DELETE pair/session):
    // the HTTP status + whether a body parsed (for the terminal-401 check).
    using AckCb = std::function<void(int httpStatus, bool reachable, const QString& code)>;

    // PUT /api/connections — declarative session upsert. `controllers` is the
    // WHOLE desired set; `mouseControl` is the requested host feature (v1
    // sends false — the touchpad-mouse UI is deferred — but still parses the
    // grant).
    void putSession(const QString& ip, int port, const QString& deviceId, const QString& deviceName,
                    const QString& hmacProof,
                    const QList<models::ControllerDescriptor>& controllers, bool mouseControl,
                    SessionCb cb);

    // GET /api/connections/{id} — the reconcile endpoint (applied state +
    // epoch).
    void getSession(const QString& ip, int port, const QString& connectionId,
                    const QString& deviceId, const QString& hmacProof, ViewCb cb);

    // DELETE /api/connections/{id} — graceful close (no notify).
    void deleteSession(const QString& ip, int port, const QString& connectionId,
                       const QString& deviceId, const QString& hmacProof, AckCb cb);

    // PUT /api/connections/{id}/controllers/{idx} — standalone
    // single-descriptor upsert; converges without rotating the token (path
    // idx wins).
    void putController(const QString& ip, int port, const QString& connectionId,
                       const QString& deviceId, const QString& hmacProof,
                       const models::ControllerDescriptor& descriptor, ControllerCb cb);

    // DELETE /api/connections/{id}/controllers/{idx} — removes the SLOT only.
    void deleteController(const QString& ip, int port, const QString& connectionId, int ctrlIdx,
                          const QString& deviceId, const QString& hmacProof, ControllerCb cb);

    // DELETE /api/pair — client self-unpair (X-Device-Id + X-Hmac-Proof).
    void unpair(const QString& ip, int port, const QString& deviceId, const QString& hmacProof,
                AckCb cb);

  private:
    // One exchange's decoded result, before route-specific DTO parsing.
    struct RawReply {
        int status = 0;         // 0 = transport never produced a response
        bool reachable = false; // a body (any) was received
        QByteArray body;
    };

    // Issue `method` with the protocol-1 headers; invoke `done` with the raw
    // result on the home thread. `body` empty = no request body.
    void perform(const QString& url, const QByteArray& method, const QByteArray& body,
                 const QString& deviceId, const QString& hmacProof,
                 std::function<void(const RawReply&)> done);

    QNetworkAccessManager* nam_;
    PinVerifier pinVerifier_;
};

} // namespace dish::net
