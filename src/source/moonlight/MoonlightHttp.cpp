// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "source/moonlight/MoonlightHttp.h"

#include "source/http/HttpExchange.h"
#include "source/moonlight/MoonlightLog.h"

#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslKey>
#include <QSslSocket>
#include <QUrl>
#include <QUuid>

#include <optional>
#include <utility>

namespace dish::source::moon {
namespace {

// Whatever answered is handed over with its status and body, a refusal as much as a success:
// status 0 is kept for nothing having answered at all.
void handOver(const QString& path, const MoonlightHttp::BodyCb& cb,
              const http::HttpResult& result) {
    const int status = result.response.status;
    if (status == 0) {
        qCWarning(lcMoon) << "http" << path << "failed:" << result.error;
    } else {
        qCDebug(lcMoon) << "http <-" << path << status << result.response.body.size() << "bytes";
    }
    cb(status, QByteArray::fromStdString(result.response.body));
}

} // namespace

MoonlightHttp::MoonlightHttp(QObject* parent) : QObject(parent) {}

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

QSslConfiguration MoonlightHttp::tlsConfiguration(const QString& certPem,
                                                  const QString& privateKeyPem) {
    QSslConfiguration ssl = QSslConfiguration::defaultConfiguration();
    // Self-signed on both ends; trust is the pin, checked the moment the
    // handshake completes.
    ssl.setPeerVerifyMode(QSslSocket::VerifyNone);
    // NEVER OFFER A SESSION TO RESUME. A resumed TLS session carries the peer
    // identity forward instead of asking for the certificate again, so a
    // Moonlight host's verify callback never runs and Sunshine kills the
    // connection with a fatal internal_error alert (RFC 8446 alert 80) and logs
    // nothing at all. Every call dials a fresh socket, and all three switches go
    // off together so that no session is cached, shared or kept for the next
    // one, whatever reuses this configuration.
    ssl.setSslOption(QSsl::SslOptionDisableSessionTickets, true);
    ssl.setSslOption(QSsl::SslOptionDisableSessionSharing, true);
    ssl.setSslOption(QSsl::SslOptionDisableSessionPersistence, true);
    const auto certs = QSslCertificate::fromData(certPem.toUtf8(), QSsl::Pem);
    if (!certs.isEmpty()) { ssl.setLocalCertificate(certs.first()); }
    QSslKey key(privateKeyPem.toUtf8(), QSsl::Rsa, QSsl::Pem, QSsl::PrivateKey);
    if (!key.isNull()) { ssl.setPrivateKey(key); }
    return ssl;
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

    http::HttpRequest request;
    request.url = full;
    request.method = QByteArrayLiteral("GET");
    request.tls = tls ? std::optional(tlsConfiguration(certPem_, privateKeyPem_)) : std::nullopt;
    request.timeoutMs = timeoutMs;
    const QString path = full.path();
    qCDebug(lcMoon) << "http ->" << (tls ? "https" : "http") << full.host() << path;
    http::exchangeHttp(
        this, std::move(request),
        [pinnedServerCertPem](const QByteArray& certDer) {
            return matchesPin(certDer, pinnedServerCertPem);
        },
        [path, cb = std::move(cb)](const http::HttpResult& result) { handOver(path, cb, result); });
}

bool MoonlightHttp::matchesPin(const QByteArray& presentedDer, const QString& pinnedPem) {
    const QByteArray pinnedDer =
        QSslCertificate::fromData(pinnedPem.toUtf8(), QSsl::Pem).value(0).toDer();
    return !pinnedDer.isEmpty() && pinnedDer == presentedDer;
}

} // namespace dish::source::moon
