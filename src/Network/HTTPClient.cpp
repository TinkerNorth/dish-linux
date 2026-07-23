// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "HTTPClient.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslError>
#include <QSslSocket>
#include <QUrl>

namespace dish::net {

namespace {

constexpr int kTimeoutMs = 5000;

// Parse a reply body into a JSON object; empty object on malformed/empty.
QJsonObject parseObject(const QByteArray& body) {
    if (body.isEmpty()) { return {}; }
    QJsonParseError err{};
    const auto doc = QJsonDocument::fromJson(body, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) { return {}; }
    return doc.object();
}

} // namespace

HTTPClient::HTTPClient(QObject* parent) : QObject(parent), nam_(new QNetworkAccessManager(this)) {
    nam_->setTransferTimeout(kTimeoutMs);
}

HTTPClient::~HTTPClient() = default;

void HTTPClient::perform(const QString& url, const QByteArray& method, const QByteArray& body,
                         const QString& deviceId, const QString& hmacProof,
                         std::function<void(const RawReply&)> done) {
    QNetworkRequest req((QUrl(url)));
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    // Every authenticated route carries the device id + a proof of the pairing
    // key, so a diverged key fails HERE with a terminal 401 instead of a
    // silently-undecryptable UDP session (contract §hmacProof).
    if (!deviceId.isEmpty()) { req.setRawHeader("X-Device-Id", deviceId.toUtf8()); }
    if (!hmacProof.isEmpty()) { req.setRawHeader("X-Hmac-Proof", hmacProof.toUtf8()); }

    // Self-signed cert: there is no CA chain to validate, so peer verification
    // is off (curl --insecure). Trust is enforced by the TOFU pin verifier on
    // the `encrypted` signal below — VerifyNone here just stops Qt from
    // refusing the self-signed chain before we get a chance to pin it.
    QSslConfiguration tls = QSslConfiguration::defaultConfiguration();
    tls.setPeerVerifyMode(QSslSocket::VerifyNone);
    req.setSslConfiguration(tls);

    const QString host = QUrl(url).host();

    auto* reply = nam_->sendCustomRequest(req, method, body);
    QObject::connect(reply, &QNetworkReply::sslErrors, reply,
                     [reply](const QList<QSslError>&) { reply->ignoreSslErrors(); });
    // TOFU pin check: once the handshake completes the peer cert is available.
    // First contact pins + proceeds; a cert whose fingerprint differs from the
    // pin aborts the request (the `finished` handler then reports it
    // unreachable, exactly as a dropped connection would). No verifier
    // installed → no-op.
    if (pinVerifier_) {
        QObject::connect(reply, &QNetworkReply::encrypted, reply, [this, reply, host] {
            const QByteArray der = reply->sslConfiguration().peerCertificate().toDer();
            if (!pinVerifier_(host, der)) { reply->abort(); }
        });
    }
    QObject::connect(reply, &QNetworkReply::finished, this, [reply, done = std::move(done)] {
        reply->deleteLater();
        RawReply out;
        const auto statusVar = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
        out.status = statusVar.isValid() ? statusVar.toInt() : 0;
        out.body = reply->readAll();
        // A transport-level failure with no HTTP status is unreachable; a body
        // (including a 4xx/5xx error body) means the server answered.
        out.reachable = out.status != 0 || !out.body.isEmpty();
        done(out);
    });
}

void HTTPClient::putSession(const QString& ip, int port, const QString& deviceId,
                            const QString& deviceName, const QString& hmacProof,
                            const QList<models::ControllerDescriptor>& controllers,
                            bool mouseControl, SessionCb cb) {
    const QString url = QStringLiteral("https://%1:%2/api/connections").arg(ip).arg(port);
    QJsonObject obj{
        {"deviceId", deviceId},
        {"deviceName", deviceName},
        {"protocolVersion", proto::kProtocolVersion},
        {"hmacProof", hmacProof},
        {"controllers", models::controllersJson(controllers)},
        {"hostFeatures", QJsonObject{{"mouseControl", mouseControl}}},
    };
    const auto body = QJsonDocument(obj).toJson(QJsonDocument::Compact);
    perform(url, "PUT", body, deviceId, hmacProof, [cb = std::move(cb)](const RawReply& r) {
        models::SessionResponse resp;
        if (r.reachable) { resp = models::SessionResponse::fromJson(parseObject(r.body)); }
        resp.httpStatus = r.status;
        resp.reachable = r.reachable;
        cb(resp);
    });
}

void HTTPClient::getSession(const QString& ip, int port, const QString& connectionId,
                            const QString& deviceId, const QString& hmacProof, ViewCb cb) {
    const QString url =
        QStringLiteral("https://%1:%2/api/connections/%3").arg(ip).arg(port).arg(connectionId);
    perform(url, "GET", {}, deviceId, hmacProof, [cb = std::move(cb)](const RawReply& r) {
        models::SessionViewDto view;
        if (r.reachable) { view = models::SessionViewDto::fromJson(parseObject(r.body)); }
        view.httpStatus = r.status;
        view.reachable = r.reachable;
        cb(view);
    });
}

void HTTPClient::getCatalog(const QString& ip, int port, CatalogCb cb) {
    const QString url = QStringLiteral("https://%1:%2/api/catalog").arg(ip).arg(port);
    // Unauthenticated route: empty deviceId/proof → perform attaches no auth
    // headers. Unreachable/old satellite → empty catalog (caller falls back).
    perform(url, "GET", {}, {}, {}, [cb = std::move(cb)](const RawReply& r) {
        models::ServerCatalog catalog;
        if (r.reachable) { catalog = models::ServerCatalog::fromJson(parseObject(r.body)); }
        cb(catalog);
    });
}

void HTTPClient::deleteSession(const QString& ip, int port, const QString& connectionId,
                               const QString& deviceId, const QString& hmacProof, AckCb cb) {
    const QString url =
        QStringLiteral("https://%1:%2/api/connections/%3").arg(ip).arg(port).arg(connectionId);
    perform(url, "DELETE", {}, deviceId, hmacProof, [cb = std::move(cb)](const RawReply& r) {
        const auto code = models::SessionResponse::fromJson(parseObject(r.body)).code;
        cb(r.status, r.reachable, code.value_or(QString()));
    });
}

void HTTPClient::putController(const QString& ip, int port, const QString& connectionId,
                               const QString& deviceId, const QString& hmacProof,
                               const models::ControllerDescriptor& descriptor, ControllerCb cb) {
    const QString url = QStringLiteral("https://%1:%2/api/connections/%3/controllers/%4")
                            .arg(ip)
                            .arg(port)
                            .arg(connectionId)
                            .arg(descriptor.ctrlIdx);
    const auto body = QJsonDocument(descriptor.toJson()).toJson(QJsonDocument::Compact);
    perform(url, "PUT", body, deviceId, hmacProof, [cb = std::move(cb)](const RawReply& r) {
        models::ControllerPutResponse resp;
        if (r.reachable) { resp = models::ControllerPutResponse::fromJson(parseObject(r.body)); }
        resp.httpStatus = r.status;
        resp.reachable = r.reachable;
        cb(resp);
    });
}

void HTTPClient::deleteController(const QString& ip, int port, const QString& connectionId,
                                  int ctrlIdx, const QString& deviceId, const QString& hmacProof,
                                  ControllerCb cb) {
    const QString url = QStringLiteral("https://%1:%2/api/connections/%3/controllers/%4")
                            .arg(ip)
                            .arg(port)
                            .arg(connectionId)
                            .arg(ctrlIdx);
    perform(url, "DELETE", {}, deviceId, hmacProof, [cb = std::move(cb)](const RawReply& r) {
        models::ControllerPutResponse resp;
        if (r.reachable) { resp = models::ControllerPutResponse::fromJson(parseObject(r.body)); }
        resp.httpStatus = r.status;
        resp.reachable = r.reachable;
        cb(resp);
    });
}

void HTTPClient::unpair(const QString& ip, int port, const QString& deviceId,
                        const QString& hmacProof, AckCb cb) {
    const QString url = QStringLiteral("https://%1:%2/api/pair").arg(ip).arg(port);
    perform(url, "DELETE", {}, deviceId, hmacProof, [cb = std::move(cb)](const RawReply& r) {
        const auto code = models::SessionResponse::fromJson(parseObject(r.body)).code;
        cb(r.status, r.reachable, code.value_or(QString()));
    });
}

} // namespace dish::net
