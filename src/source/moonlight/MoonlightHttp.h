// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Async gateway to a Moonlight host's GameStream HTTP API: plain HTTP (47989)
// for serverinfo and the pairing phases, HTTPS (47984) with the client
// certificate for everything after. The host's cert is self-signed, so peer
// verification is off and trust is the pairing-time pin: every TLS reply is
// checked against the certificate the pairing handshake verified, and a
// mismatch is reported as unreachable rather than handing bytes from an
// imposter to the caller.
//
// Callbacks fire on the manager's home thread (the Qt main thread).

#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QUrlQuery>

#include <functional>

class QNetworkAccessManager;

namespace dish::source::moon {

class MoonlightHttp : public QObject {
    Q_OBJECT
  public:
    explicit MoonlightHttp(QObject* parent = nullptr);
    ~MoonlightHttp() override;

    // The client identity every TLS call presents, plus the uniqueid query
    // parameter every GameStream call carries.
    void setIdentity(const QString& certPem, const QString& privateKeyPem, const QString& uniqueId);
    QString uniqueId() const { return uniqueId_; }

    // status 0 = the transport never produced a response (includes a TLS pin
    // mismatch); the body is then empty.
    using BodyCb = std::function<void(int status, const QByteArray& body)>;

    // GET http://address:port/path?uniqueid=...&uuid=...&<query>.
    // `timeoutMs` exists because pairing phase 1 legitimately blocks until the
    // user types the PIN into the host.
    void getPlain(const QString& address, int port, const QString& path, const QUrlQuery& query,
                  BodyCb cb, int timeoutMs = kDefaultTimeoutMs);

    // GET https://... with the client cert; the reply is accepted only when
    // the presented server certificate matches `pinnedServerCertPem`.
    void getTls(const QString& address, int port, const QString& path, const QUrlQuery& query,
                const QString& pinnedServerCertPem, BodyCb cb, int timeoutMs = kDefaultTimeoutMs);

    static constexpr int kDefaultTimeoutMs = 10000;
    static constexpr int kPairingTimeoutMs = 120000;

  private:
    void perform(const QUrl& url, bool tls, const QString& pinnedServerCertPem, BodyCb cb,
                 int timeoutMs);

    QNetworkAccessManager* nam_;
    QString certPem_;
    QString privateKeyPem_;
    QString uniqueId_;
};

} // namespace dish::source::moon
