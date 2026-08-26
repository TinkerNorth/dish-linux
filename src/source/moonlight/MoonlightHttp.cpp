// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "source/moonlight/MoonlightHttp.h"

#include "source/moonlight/MoonlightLog.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslKey>
#include <QUrl>
#include <QUuid>

namespace dish::source::moon {
namespace {

// Certificate equality that survives PEM reserialization differences: compare
// the DER bytes, not the text.
bool sameCert(const QSslCertificate& presented, const QString& pinnedPem) {
    if (presented.isNull() || pinnedPem.isEmpty()) { return false; }
    const auto pinned = QSslCertificate::fromData(pinnedPem.toUtf8(), QSsl::Pem);
    if (pinned.isEmpty() || pinned.first().isNull()) { return false; }
    return presented.toDer() == pinned.first().toDer();
}

} // namespace

MoonlightHttp::MoonlightHttp(QObject* parent)
    : QObject(parent), nam_(new QNetworkAccessManager(this)) {}

MoonlightHttp::~MoonlightHttp() = default;

void MoonlightHttp::setIdentity(const QString& certPem, const QString& privateKeyPem,
                                const QString& uniqueId) {
    certPem_ = certPem;
    privateKeyPem_ = privateKeyPem;
    uniqueId_ = uniqueId;
}

void MoonlightHttp::getPlain(const QString& address, int port, const QString& path,
                             const QUrlQuery& query, BodyCb cb, int timeoutMs) {
    QUrl url;
    url.setScheme(QStringLiteral("http"));
    url.setHost(address);
    url.setPort(port);
    url.setPath(path);
    url.setQuery(query);
    perform(url, false, QString(), std::move(cb), timeoutMs);
}

void MoonlightHttp::getTls(const QString& address, int port, const QString& path,
                           const QUrlQuery& query, const QString& pinnedServerCertPem, BodyCb cb,
                           int timeoutMs) {
    QUrl url;
    url.setScheme(QStringLiteral("https"));
    url.setHost(address);
    url.setPort(port);
    url.setPath(path);
    url.setQuery(query);
    perform(url, true, pinnedServerCertPem, std::move(cb), timeoutMs);
}

void MoonlightHttp::perform(const QUrl& url, bool tls, const QString& pinnedServerCertPem,
                            BodyCb cb, int timeoutMs) {
    // Every GameStream request carries the client's uniqueid plus a per-call
    // uuid nonce; hosts key caches and pairing state on the former.
    QUrl full = url;
    QUrlQuery query(full.query());
    query.addQueryItem(QStringLiteral("uniqueid"), uniqueId_);
    query.addQueryItem(QStringLiteral("uuid"),
                       QUuid::createUuid().toString(QUuid::WithoutBraces).remove(QChar('-')));
    full.setQuery(query);

    QNetworkRequest request(full);
    request.setTransferTimeout(timeoutMs);
    // GameStream hosts speak bare HTTP/1.1 and choke on upgrade probing.
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);

    if (tls) {
        QSslConfiguration ssl = QSslConfiguration::defaultConfiguration();
        // Self-signed on both ends; trust is the explicit pin check below.
        ssl.setPeerVerifyMode(QSslSocket::VerifyNone);
        // NEVER OFFER A SESSION TO RESUME. A resumed TLS session carries the
        // peer identity forward instead of asking for the certificate again,
        // so a Moonlight host's verify callback never runs and Sunshine kills
        // the connection with a fatal internal_error alert (RFC 8446 alert 80)
        // and logs nothing at all. Qt shares and persists sessions across the
        // connections one QNetworkAccessManager makes, which is exactly the
        // shape that triggers it, so all three switches go off together.
        ssl.setSslOption(QSsl::SslOptionDisableSessionTickets, true);
        ssl.setSslOption(QSsl::SslOptionDisableSessionSharing, true);
        ssl.setSslOption(QSsl::SslOptionDisableSessionPersistence, true);
        const auto certs = QSslCertificate::fromData(certPem_.toUtf8(), QSsl::Pem);
        if (!certs.isEmpty()) { ssl.setLocalCertificate(certs.first()); }
        QSslKey key(privateKeyPem_.toUtf8(), QSsl::Rsa, QSsl::Pem, QSsl::PrivateKey);
        if (!key.isNull()) { ssl.setPrivateKey(key); }
        request.setSslConfiguration(ssl);
    }

    const QString path = full.path();
    qCDebug(lcMoon) << "http ->" << (tls ? "https" : "http") << full.host() << path;
    QNetworkReply* reply = nam_->get(request);
    if (tls) {
        QObject::connect(reply, &QNetworkReply::sslErrors, reply,
                         [reply](const QList<QSslError>&) { reply->ignoreSslErrors(); });
    }
    QObject::connect(reply, &QNetworkReply::finished, this,
                     [reply, tls, path, pinnedServerCertPem, cb = std::move(cb)]() {
                         reply->deleteLater();
                         if (tls) {
                             const auto presented = reply->sslConfiguration().peerCertificate();
                             if (!sameCert(presented, pinnedServerCertPem)) {
                                 qCWarning(lcMoon) << "http" << path
                                                   << "rejected: server certificate does not "
                                                      "match the pairing pin";
                                 cb(0, QByteArray());
                                 return;
                             }
                         }
                         if (reply->error() != QNetworkReply::NoError &&
                             reply->error() != QNetworkReply::ProtocolInvalidOperationError) {
                             qCWarning(lcMoon)
                                 << "http" << path << "failed:" << reply->errorString();
                             cb(0, QByteArray());
                             return;
                         }
                         const int status =
                             reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                         const QByteArray body = reply->readAll();
                         qCDebug(lcMoon) << "http <-" << path << status << body.size() << "bytes";
                         cb(status, body);
                     });
}

} // namespace dish::source::moon
