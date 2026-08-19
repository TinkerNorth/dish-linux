// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "PairingClient.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSslConfiguration>
#include <QSslError>
#include <QSslSocket>
#include <QString>
#include <QTimer>
#include <QUrl>

#include <utility>

namespace dish::net {

namespace {

constexpr const char* kTrContext = "dish::net::PairingClient";

constexpr int kTimeoutMs = 5000;

models::PairResponse makeError(const char* msg) {
    models::PairResponse r;
    r.ok = false;
    r.error = QString::fromLatin1(msg);
    // Unreachable by construction: no JSON body ever arrived, so httpStatus also
    // stays 0. fromJson flips this on the success path.
    r.reachable = false;
    return r;
}

struct BlockingReply {
    int status = 0;
    QByteArray body;
    bool reachable = false;
};

BlockingReply blockingRequest(const QString& url, const QByteArray& method, const QByteArray& body,
                              const QString& deviceId, const QString& hmacProof,
                              const PairingClient::PinVerifier& pinVerify) {
    const QUrl parsed(url);
    QNetworkRequest req(parsed);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    if (!deviceId.isEmpty()) { req.setRawHeader("X-Device-Id", deviceId.toUtf8()); }
    if (!hmacProof.isEmpty()) { req.setRawHeader("X-Hmac-Proof", hmacProof.toUtf8()); }

    // Qt chain verification stays off because the cert is self-signed by design;
    // the real gate is the TOFU pin check on the `encrypted` edge below. The
    // manager is stack-local so it and the reply belong to this worker thread,
    // and the nested event loop pumps their signals.
    QSslConfiguration tls = QSslConfiguration::defaultConfiguration();
    tls.setPeerVerifyMode(QSslSocket::VerifyNone);
    req.setSslConfiguration(tls);

    QNetworkAccessManager nam;
    nam.setTransferTimeout(kTimeoutMs);

    QEventLoop loop;
    QNetworkReply* reply = body.isEmpty() ? nam.sendCustomRequest(req, method)
                                          : nam.sendCustomRequest(req, method, body);
    QObject::connect(reply, &QNetworkReply::sslErrors, reply,
                     [reply](const QList<QSslError>&) { reply->ignoreSslErrors(); });
    // TOFU: inspect the peer cert the moment the handshake completes, before the
    // PIN or the proof transits. A mismatch aborts, which reads back as
    // unreachable, so pairing fails safe.
    const QString host = parsed.host();
    QObject::connect(reply, &QNetworkReply::encrypted, reply, [reply, host, &pinVerify] {
        if (!pinVerify) { return; }
        const QByteArray der = reply->sslConfiguration().peerCertificate().toDer();
        if (!pinVerify(host, der)) { reply->abort(); }
    });
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);

    QTimer guard;
    guard.setSingleShot(true);
    QObject::connect(&guard, &QTimer::timeout, &loop, &QEventLoop::quit);
    guard.start(kTimeoutMs + 1000);
    loop.exec();

    BlockingReply out;
    if (!reply->isFinished()) {
        reply->abort();
        reply->deleteLater();
        return out; // status 0, unreachable
    }
    const auto statusVar = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
    out.status = statusVar.isValid() ? statusVar.toInt() : 0;
    out.body = reply->readAll();
    out.reachable = out.status != 0 || !out.body.isEmpty();
    reply->deleteLater();
    return out;
}

QJsonObject parseObject(const QByteArray& body) {
    if (body.isEmpty()) { return {}; }
    QJsonParseError err{};
    const auto doc = QJsonDocument::fromJson(body, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) { return {}; }
    return doc.object();
}

} // namespace

PairingClient::Outcome PairingClient::classify(const models::PairResponse& response) {
    reducer::PairReply r;
    r.status = response.httpStatus;
    r.bodyParsed = response.reachable;
    r.ok = response.ok;
    r.pending = response.pending;
    r.hasSharedKey = response.sharedKey.has_value() && !response.sharedKey->isEmpty();
    switch (reducer::classifyPair(r)) {
    case reducer::PairVerdict::Success:
        return Success{*response.sharedKey};
    case reducer::PairVerdict::Pending:
        return Pending{};
    case reducer::PairVerdict::AuthRequired:
        return AuthRequired{};
    case reducer::PairVerdict::VersionMismatch:
        return VersionMismatch{};
    case reducer::PairVerdict::Unreachable:
        break;
    }
    return Unreachable{
        response.error.value_or(QCoreApplication::translate(kTrContext, "Server unreachable"))};
}

models::PairResponse PairingClient::pair(const QString& ip, int port, const QString& deviceId,
                                         const QString& deviceName, const QString& pin,
                                         const QString& clientPin) {
    const QString url = QStringLiteral("https://%1:%2/api/pair").arg(ip).arg(port);
    const QJsonObject reqObj{
        {"deviceId", deviceId},
        {"deviceName", deviceName},
        {"protocolVersion", proto::kProtocolVersion},
        {"pin", pin},
        {"clientPin", clientPin},
    };
    const auto body = QJsonDocument(reqObj).toJson(QJsonDocument::Compact);
    const auto reply = blockingRequest(url, "POST", body, {}, {}, pinVerifier());
    if (!reply.reachable) { return makeError("connect failed"); }
    auto r = models::PairResponse::fromJson(parseObject(reply.body));
    r.httpStatus = reply.status;
    return r;
}

models::PairResponse PairingClient::rotateKey(const QString& ip, int port, const QString& deviceId,
                                              const QString& deviceName, const QString& hmacProof) {
    const QString url = QStringLiteral("https://%1:%2/api/pair").arg(ip).arg(port);
    const QJsonObject reqObj{
        {"deviceId", deviceId},
        {"deviceName", deviceName},
        {"protocolVersion", proto::kProtocolVersion},
        {"hmacProof", hmacProof},
    };
    const auto body = QJsonDocument(reqObj).toJson(QJsonDocument::Compact);
    const auto reply = blockingRequest(url, "POST", body, {}, {}, pinVerifier());
    if (!reply.reachable) { return makeError("connect failed"); }
    auto r = models::PairResponse::fromJson(parseObject(reply.body));
    r.httpStatus = reply.status;
    return r;
}

models::PairResponse PairingClient::pairStatus(const QString& ip, int port,
                                               const QString& deviceId) {
    const QString url = QStringLiteral("https://%1:%2/api/pair/status?deviceId=%3")
                            .arg(ip)
                            .arg(port)
                            .arg(QString::fromUtf8(QUrl::toPercentEncoding(deviceId)));
    const auto reply = blockingRequest(url, "GET", {}, {}, {}, pinVerifier());
    if (!reply.reachable) { return makeError("connect failed"); }
    auto r = models::PairResponse::fromStatusJson(parseObject(reply.body));
    r.httpStatus = reply.status;
    return r;
}

PairingClient::PinVerifier& PairingClient::pinVerifier() {
    // Set once at manager construction; worker threads only read it afterwards.
    static PinVerifier verifier;
    return verifier;
}

void PairingClient::setPinVerifier(PinVerifier verifier) { pinVerifier() = std::move(verifier); }

} // namespace dish::net
