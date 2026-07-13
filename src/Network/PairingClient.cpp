// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "PairingClient.h"

#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSslConfiguration>
#include <QSslSocket>
#include <QTimer>
#include <QUrl>

namespace dish::net {

namespace {

constexpr int kTimeoutMs = 5000;

models::PairResponse makeError(const char* msg) {
    models::PairResponse r;
    r.ok = false;
    r.error = QString::fromLatin1(msg);
    // Synthesized network-error responses are unreachable by construction —
    // we never made it far enough to receive a JSON body. fromJson flips this
    // to true on the success path.
    r.reachable = false;
    return r;
}

} // namespace

PairingClient::Outcome PairingClient::classify(const models::PairResponse& response) {
    if (response.ok && response.sharedKey.has_value() && !response.sharedKey->isEmpty()) {
        return Success{*response.sharedKey};
    }
    if (response.reachable) { return AuthRequired{}; }
    return Unreachable{response.error.value_or(QStringLiteral("Server unreachable"))};
}

models::PairResponse PairingClient::pair(const QString& ip, int port, const QString& deviceId,
                                         const QString& deviceName, const QString& pin,
                                         const PinVerifier& verifier) {
    // pair() is a blocking call invoked from a worker thread. We drive the
    // async QNetworkAccessManager request with a local QEventLoop so the
    // function keeps its blocking contract. The manager and event loop are
    // both local to this thread/stack, which is the supported way to use
    // QNetworkAccessManager off the main thread.
    QNetworkAccessManager nam;
    nam.setTransferTimeout(kTimeoutMs);

    const QString url = QStringLiteral("https://%1:%2/api/pair").arg(ip).arg(port);
    QNetworkRequest req((QUrl(url)));
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    // Self-signed cert: no CA chain to validate, so peer verification is off —
    // trust is enforced by the TOFU pin verifier on the `encrypted` signal
    // below. VerifyNone just stops Qt from refusing the self-signed chain
    // before we get a chance to pin it.
    QSslConfiguration tls = QSslConfiguration::defaultConfiguration();
    tls.setPeerVerifyMode(QSslSocket::VerifyNone);
    req.setSslConfiguration(tls);

    const QJsonObject reqObj{{"deviceId", deviceId},
                             {"deviceName", deviceName},
                             {"pin", pin},
                             {"protocolVersion", proto::kProtocolVersion}};
    const auto body = QJsonDocument(reqObj).toJson(QJsonDocument::Compact);

    QNetworkReply* reply = nam.post(req, body);

    QObject::connect(reply, &QNetworkReply::sslErrors, reply,
                     [reply](const QList<QSslError>&) { reply->ignoreSslErrors(); });
    // TOFU pin check: first contact pins + proceeds; a cert whose fingerprint
    // differs from the stored pin aborts the pair — the classifier then
    // reports Unreachable, exactly like a dropped connection.
    if (verifier) {
        QObject::connect(reply, &QNetworkReply::encrypted, reply, [reply, &verifier, ip] {
            const QByteArray der = reply->sslConfiguration().peerCertificate().toDer();
            if (!verifier(ip, der)) { reply->abort(); }
        });
    }

    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    // Hard ceiling in case the transfer timeout does not fire (e.g. stalled
    // mid-stream); abort() emits finished() and unblocks the loop.
    QTimer::singleShot(kTimeoutMs + 2000, reply, [reply] {
        if (reply->isRunning()) { reply->abort(); }
    });
    loop.exec();

    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        // Transport failure — never reached a JSON body, so leave reachable
        // false so the Outcome classifier reports Unreachable.
        return makeError("no response");
    }

    QJsonParseError err{};
    const auto doc = QJsonDocument::fromJson(reply->readAll(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        return makeError("malformed response");
    }
    return models::PairResponse::fromJson(doc.object());
}

} // namespace dish::net
