// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#pragma once

#include "Models/Models.h"
#include "core/reducer/RestOutcome.h"

#include <QByteArray>
#include <QObject>
#include <QString>

#include <functional>

namespace dish::http {
struct HttpResult;
}

namespace dish::net {

// Async gateway to the satellite's REST API (HTTPS :9443). The caller computes
// the `hmacProof` argument via core/wire/SessionCrypto. Callbacks fire from the
// event loop of the thread the client lives on, which is the Qt main thread.
//
// The satellite's cert is self-signed, so there is no CA chain and peer
// verification stays VerifyNone; trust comes entirely from the TOFU pin verifier
// below. Without a verifier installed the connection is unauthenticated.
class HTTPClient : public QObject {
    Q_OBJECT
  public:
    explicit HTTPClient(QObject* parent = nullptr);
    ~HTTPClient() override;

    // Returning false aborts the request. Pins on first contact and rejects a
    // later cert whose fingerprint differs. See source/http/SatelliteTlsVerifier.
    // `pinMismatch` is set only for that CHANGED-cert case, never for a missing
    // cert, so the caller can tell an identity change from a dead link.
    using PinVerifier =
        std::function<bool(const QString& host, const QByteArray& certDer, bool& pinMismatch)>;
    void setPinVerifier(PinVerifier verifier) { pinVerifier_ = std::move(verifier); }

    // The mismatch rides beside the DTO because an aborted handshake leaves no
    // body to carry it.
    using SessionCb = std::function<void(const models::SessionResponse&, bool pinMismatch)>;
    using ControllerCb = std::function<void(const models::ControllerPutResponse&)>;
    using ViewCb = std::function<void(const models::SessionViewDto&)>;
    using CapabilitiesCb = std::function<void(const models::CapabilitiesDto&)>;
    using CatalogCb = std::function<void(const models::CatalogDto&)>;
    // For routes the caller does not decode; `reachable` distinguishes a real
    // 401 from a dead transport.
    using AckCb = std::function<void(int httpStatus, bool reachable, const QString& code)>;
    // A pairing reply, with the mismatch beside it for the same reason as SessionCb's.
    using PairCb = std::function<void(const models::PairResponse&, bool pinMismatch)>;

    // Declarative upsert: `controllers` must be the WHOLE desired set, not a
    // delta. `mouseControl` is always false today (no touchpad-mouse UI) but the
    // grant is still parsed back.
    //
    // `protocolVersion` is the version to OFFER. Normally proto::kProtocolVersion;
    // a satellite that answered 409 with a lower ceiling gets re-offered that
    // ceiling instead of the same rejected number (see ProtocolNegotiation.h).
    void putSession(const QString& ip, int port, const QString& deviceId, const QString& deviceName,
                    const QString& hmacProof,
                    const QList<models::ControllerDescriptor>& controllers, bool mouseControl,
                    int protocolVersion, SessionCb cb);

    // GET /api/connections/{id} — the reconcile endpoint (applied state + epoch).
    void getSession(const QString& ip, int port, const QString& connectionId,
                    const QString& deviceId, const QString& hmacProof, ViewCb cb);

    // DELETE /api/connections/{id} — graceful close (no notify).
    void deleteSession(const QString& ip, int port, const QString& connectionId,
                       const QString& deviceId, const QString& hmacProof, AckCb cb);

    // PUT /api/connections/{id}/controllers/{idx} — single-descriptor upsert that
    // converges without rotating the session token. The path idx wins over the
    // descriptor's.
    void putController(const QString& ip, int port, const QString& connectionId,
                       const QString& deviceId, const QString& hmacProof,
                       const models::ControllerDescriptor& descriptor, ControllerCb cb);

    // DELETE /api/connections/{id}/controllers/{idx} — removes the SLOT only.
    void deleteController(const QString& ip, int port, const QString& connectionId, int ctrlIdx,
                          const QString& deviceId, const QString& hmacProof, ControllerCb cb);

    // POST /api/pair. Path A (operator `pin`) and Path B (client-shown `clientPin`,
    // which answers Pending and is then polled). Both fields always ride in the
    // body, empty when unused; the server tries a valid `pin` first. It passes the
    // same pin gate as every other call here: the first pair pins the certificate,
    // and every later call must present it.
    void pair(const QString& ip, int port, const QString& deviceId, const QString& deviceName,
              const QString& pin, const QString& clientPin, PairCb cb);

    // GET /api/pair/status?deviceId= — the Path B approval poll.
    void pairStatus(const QString& ip, int port, const QString& deviceId, PairCb cb);

    // DELETE /api/pair — client self-unpair (X-Device-Id + X-Hmac-Proof).
    void unpair(const QString& ip, int port, const QString& deviceId, const QString& hmacProof,
                AckCb cb);

    // GET /api/server/capabilities — unauthenticated backend health.
    void getCapabilities(const QString& ip, int port, CapabilitiesCb cb);

    // GET /api/catalog — unauthenticated, localized by `acceptLanguage`. A
    // non-empty `etag` rides If-None-Match, so a 304 means serve the cache.
    void getCatalog(const QString& ip, int port, const QString& acceptLanguage, const QString& etag,
                    CatalogCb cb);

  private:
    struct RawReply {
        int status = 0;         // 0 = the transport never produced a response
        bool reachable = false; // the server answered, even if with an error
        QByteArray body;
        QString etag;
        bool pinMismatch = false;
        // Why the transport failed, when it did. None whenever `reachable`.
        // Diagnostic only: no retry or verdict reads it, so it cannot change a
        // decision that the status and body did not already make.
        reducer::TransportFailure failure = reducer::TransportFailure::None;
    };

    static RawReply rawReplyOf(const http::HttpResult& result, bool pinMismatch);

    void perform(const QString& url, const QByteArray& method, const QByteArray& body,
                 const QString& deviceId, const QString& hmacProof, const QString& acceptLanguage,
                 const QString& ifNoneMatch, std::function<void(const RawReply&)> done);

    PinVerifier pinVerifier_;
};

} // namespace dish::net
