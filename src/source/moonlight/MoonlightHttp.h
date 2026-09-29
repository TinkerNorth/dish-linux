// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Async gateway to a Moonlight host's GameStream HTTP API: plain HTTP (47989)
// for serverinfo and the pairing phases, HTTPS (47984) with the client
// certificate for everything after. The host's cert is self-signed, so peer
// verification is off and trust is the pairing-time pin: every TLS call checks
// the certificate the host presents against the one the pairing handshake
// verified before a byte of the request is written, and a mismatch is reported
// as unreachable rather than handing a request to an imposter.
//
// Callbacks fire from the event loop of the thread this lives on (the Qt main
// thread).

#pragma once

#include <QByteArray>
#include <QObject>
#include <QSslConfiguration>
#include <QString>
#include <QUrlQuery>

#include <functional>

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

    // The TLS configuration every mutual-TLS call presents. Exposed because the
    // one thing it must guarantee cannot be observed any other way: a resumed
    // session skips a Moonlight host's verify callback, and the host answers
    // that with a fatal alert and no log line at all.
    static QSslConfiguration tlsConfiguration(const QString& certPem, const QString& privateKeyPem);

    // Whether the certificate a host presented (DER) is the one pinned at pairing (PEM). Compared
    // as DER, so a PEM that was re-serialized still matches; nothing pinned matches nothing.
    static bool matchesPin(const QByteArray& presentedDer, const QString& pinnedPem);

    static constexpr int kDefaultTimeoutMs = 10000;
    static constexpr int kPairingTimeoutMs = 120000;

  private:
    void perform(const QUrl& url, bool tls, const QString& pinnedServerCertPem, BodyCb cb,
                 int timeoutMs);

    QString certPem_;
    QString privateKeyPem_;
    QString uniqueId_;
};

} // namespace dish::source::moon
