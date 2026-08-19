// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#pragma once

#include "Models/Models.h"

#include <QByteArray>
#include <QObject>
#include <QString>

#include <functional>

class QNetworkAccessManager;
class QNetworkReply;

namespace dish::net {

// Async gateway to the satellite's REST API (HTTPS :9443). The caller computes
// the `hmacProof` argument via core/wire/SessionCrypto. Callbacks fire on the
// network manager's home thread, which is the Qt main thread.
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
    using PinVerifier = std::function<bool(const QString& host, const QByteArray& certDer)>;
    void setPinVerifier(PinVerifier verifier) { pinVerifier_ = std::move(verifier); }

    using SessionCb = std::function<void(const models::SessionResponse&)>;
    using ControllerCb = std::function<void(const models::ControllerPutResponse&)>;
    using ViewCb = std::function<void(const models::SessionViewDto&)>;
    using CapabilitiesCb = std::function<void(const models::CapabilitiesDto&)>;
    using CatalogCb = std::function<void(const models::CatalogDto&)>;
    // For routes the caller does not decode; `reachable` distinguishes a real
    // 401 from a dead transport.
    using AckCb = std::function<void(int httpStatus, bool reachable, const QString& code)>;

    // Declarative upsert: `controllers` must be the WHOLE desired set, not a
    // delta. `mouseControl` is always false today (no touchpad-mouse UI) but the
    // grant is still parsed back.
    void putSession(const QString& ip, int port, const QString& deviceId, const QString& deviceName,
                    const QString& hmacProof,
                    const QList<models::ControllerDescriptor>& controllers, bool mouseControl,
                    SessionCb cb);

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
    };

    void perform(const QString& url, const QByteArray& method, const QByteArray& body,
                 const QString& deviceId, const QString& hmacProof, const QString& acceptLanguage,
                 const QString& ifNoneMatch, std::function<void(const RawReply&)> done);

    QNetworkAccessManager* nam_;
    PinVerifier pinVerifier_;
};

} // namespace dish::net
