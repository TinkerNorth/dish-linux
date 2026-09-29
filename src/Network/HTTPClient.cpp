// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "HTTPClient.h"

#include "source/http/HttpExchange.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSslConfiguration>
#include <QSslSocket>
#include <QUrl>

#include <memory>
#include <utility>
#include <vector>

namespace dish::net {

namespace {

constexpr int kTimeoutMs = 5000;

// VerifyNone only stops the handshake refusing the self-signed certificate before the pin verifier
// has seen it; the verifier is the real gate.
QSslConfiguration satelliteTls() {
    QSslConfiguration tls = QSslConfiguration::defaultConfiguration();
    tls.setPeerVerifyMode(QSslSocket::VerifyNone);
    return tls;
}

// Every call is JSON, and the rest ride only when they carry something. The proof header exists so
// a diverged pairing key fails with a terminal 401 rather than as a silently undecryptable UDP
// session.
std::vector<wire::HttpHeader> satelliteHeaders(const QString& deviceId, const QString& hmacProof,
                                               const QString& acceptLanguage,
                                               const QString& ifNoneMatch) {
    struct Field {
        const char* name;
        const QString& value;
    };
    const Field optionalFields[] = {{"X-Device-Id", deviceId},
                                    {"X-Hmac-Proof", hmacProof},
                                    {"Accept-Language", acceptLanguage},
                                    {"If-None-Match", ifNoneMatch}};
    std::vector<wire::HttpHeader> headers{{"Content-Type", "application/json"}};
    for (const Field& field : optionalFields) {
        if (!field.value.isEmpty()) { headers.emplace_back(field.name, field.value.toStdString()); }
    }
    return headers;
}

QJsonObject parseObject(const QByteArray& body) {
    if (body.isEmpty()) { return {}; }
    QJsonParseError err{};
    const auto doc = QJsonDocument::fromJson(body, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) { return {}; }
    return doc.object();
}

} // namespace

HTTPClient::HTTPClient(QObject* parent) : QObject(parent) {}

HTTPClient::~HTTPClient() = default;

// A status is the server answering, whatever it said; without one the transport failed, and the
// failure says how.
HTTPClient::RawReply HTTPClient::rawReplyOf(const http::HttpResult& result, bool pinMismatch) {
    RawReply out;
    out.status = result.response.status;
    out.reachable = out.status != 0;
    out.body = QByteArray::fromStdString(result.response.body);
    out.etag = QString::fromStdString(result.response.header("etag"));
    out.pinMismatch = pinMismatch;
    out.failure = result.failure;
    return out;
}

void HTTPClient::perform(const QString& url, const QByteArray& method, const QByteArray& body,
                         const QString& deviceId, const QString& hmacProof,
                         const QString& acceptLanguage, const QString& ifNoneMatch,
                         std::function<void(const RawReply&)> done) {
    http::HttpRequest request{
        .url = QUrl(url),
        .method = method,
        .headers = satelliteHeaders(deviceId, hmacProof, acceptLanguage, ifNoneMatch),
        .body = body,
        .tls = satelliteTls(),
        .timeoutMs = kTimeoutMs};
    // The pin is checked on the TLS `encrypted` edge, and a refusal there leaves no status and no
    // body, so the mismatch rides out beside the reply or it would read as a dropped connection.
    const auto pinMismatch = std::make_shared<bool>(false);
    const QString host = request.url.host();
    http::PeerCheck peerCheck;
    if (pinVerifier_) {
        peerCheck = [this, host, pinMismatch](const QByteArray& certDer) {
            return pinVerifier_(host, certDer, *pinMismatch);
        };
    }
    http::exchangeHttp(this, std::move(request), std::move(peerCheck),
                       [pinMismatch, done = std::move(done)](const http::HttpResult& result) {
                           done(rawReplyOf(result, *pinMismatch));
                       });
}

void HTTPClient::putSession(const QString& ip, int port, const QString& deviceId,
                            const QString& deviceName, const QString& hmacProof,
                            const QList<models::ControllerDescriptor>& controllers,
                            bool mouseControl, int protocolVersion, SessionCb cb) {
    const QString url = QStringLiteral("https://%1:%2/api/connections").arg(ip).arg(port);
    QJsonObject obj{
        {"deviceId", deviceId},
        {"deviceName", deviceName},
        {"protocolVersion", protocolVersion},
        {"hmacProof", hmacProof},
        {"controllers", models::controllersJson(controllers)},
        {"hostFeatures", QJsonObject{{"mouseControl", mouseControl}}},
    };
    const auto body = QJsonDocument(obj).toJson(QJsonDocument::Compact);
    perform(url, "PUT", body, deviceId, hmacProof, {}, {}, [cb = std::move(cb)](const RawReply& r) {
        models::SessionResponse resp;
        if (r.reachable) { resp = models::SessionResponse::fromJson(parseObject(r.body)); }
        resp.httpStatus = r.status;
        resp.reachable = r.reachable;
        resp.failure = r.failure;
        cb(resp, r.pinMismatch);
    });
}

void HTTPClient::getSession(const QString& ip, int port, const QString& connectionId,
                            const QString& deviceId, const QString& hmacProof, ViewCb cb) {
    const QString url =
        QStringLiteral("https://%1:%2/api/connections/%3").arg(ip).arg(port).arg(connectionId);
    perform(url, "GET", {}, deviceId, hmacProof, {}, {}, [cb = std::move(cb)](const RawReply& r) {
        models::SessionViewDto view;
        if (r.reachable) { view = models::SessionViewDto::fromJson(parseObject(r.body)); }
        view.httpStatus = r.status;
        view.reachable = r.reachable;
        cb(view);
    });
}

void HTTPClient::deleteSession(const QString& ip, int port, const QString& connectionId,
                               const QString& deviceId, const QString& hmacProof, AckCb cb) {
    const QString url =
        QStringLiteral("https://%1:%2/api/connections/%3").arg(ip).arg(port).arg(connectionId);
    perform(url, "DELETE", {}, deviceId, hmacProof, {}, {},
            [cb = std::move(cb)](const RawReply& r) {
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
    perform(url, "PUT", body, deviceId, hmacProof, {}, {}, [cb = std::move(cb)](const RawReply& r) {
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
    perform(url, "DELETE", {}, deviceId, hmacProof, {}, {},
            [cb = std::move(cb)](const RawReply& r) {
                models::ControllerPutResponse resp;
                if (r.reachable) {
                    resp = models::ControllerPutResponse::fromJson(parseObject(r.body));
                }
                resp.httpStatus = r.status;
                resp.reachable = r.reachable;
                cb(resp);
            });
}

namespace {

// A reply that never arrived is "connect failed" whatever the endpoint; one that did is read by the
// endpoint's own parser and stamped with the transport status, which the body cannot carry.
models::PairResponse pairResponseFrom(const QByteArray& body, int status, bool reachable,
                                      models::PairResponse (*parse)(const QJsonObject&)) {
    if (!reachable) {
        models::PairResponse r;
        r.ok = false;
        r.error = QStringLiteral("connect failed");
        // No JSON body arrived, so httpStatus stays 0 as well.
        r.reachable = false;
        return r;
    }
    auto r = parse(parseObject(body));
    r.httpStatus = status;
    return r;
}

} // namespace

void HTTPClient::pair(const QString& ip, int port, const QString& deviceId,
                      const QString& deviceName, const QString& pin, const QString& clientPin,
                      PairCb cb) {
    const QString url = QStringLiteral("https://%1:%2/api/pair").arg(ip).arg(port);
    const QJsonObject obj{
        {"deviceId", deviceId},
        {"deviceName", deviceName},
        {"protocolVersion", proto::kProtocolVersion},
        {"pin", pin},
        {"clientPin", clientPin},
    };
    perform(url, "POST", QJsonDocument(obj).toJson(QJsonDocument::Compact), {}, {}, {}, {},
            [cb = std::move(cb)](const RawReply& r) {
                cb(pairResponseFrom(r.body, r.status, r.reachable, &models::PairResponse::fromJson),
                   r.pinMismatch);
            });
}

void HTTPClient::pairStatus(const QString& ip, int port, const QString& deviceId, PairCb cb) {
    const QString url = QStringLiteral("https://%1:%2/api/pair/status?deviceId=%3")
                            .arg(ip)
                            .arg(port)
                            .arg(QString::fromUtf8(QUrl::toPercentEncoding(deviceId)));
    perform(url, "GET", {}, {}, {}, {}, {}, [cb = std::move(cb)](const RawReply& r) {
        cb(pairResponseFrom(r.body, r.status, r.reachable, &models::PairResponse::fromStatusJson),
           r.pinMismatch);
    });
}

void HTTPClient::unpair(const QString& ip, int port, const QString& deviceId,
                        const QString& hmacProof, AckCb cb) {
    const QString url = QStringLiteral("https://%1:%2/api/pair").arg(ip).arg(port);
    perform(url, "DELETE", {}, deviceId, hmacProof, {}, {},
            [cb = std::move(cb)](const RawReply& r) {
                const auto code = models::SessionResponse::fromJson(parseObject(r.body)).code;
                cb(r.status, r.reachable, code.value_or(QString()));
            });
}

void HTTPClient::getCapabilities(const QString& ip, int port, CapabilitiesCb cb) {
    const QString url = QStringLiteral("https://%1:%2/api/server/capabilities").arg(ip).arg(port);
    perform(url, "GET", {}, {}, {}, {}, {}, [cb = std::move(cb)](const RawReply& r) {
        models::CapabilitiesDto caps;
        if (r.reachable) { caps = models::CapabilitiesDto::fromJson(parseObject(r.body)); }
        caps.httpStatus = r.status;
        caps.reachable = r.reachable;
        cb(caps);
    });
}

void HTTPClient::getCatalog(const QString& ip, int port, const QString& acceptLanguage,
                            const QString& etag, CatalogCb cb) {
    const QString url = QStringLiteral("https://%1:%2/api/catalog").arg(ip).arg(port);
    perform(url, "GET", {}, {}, {}, acceptLanguage, etag, [cb = std::move(cb)](const RawReply& r) {
        models::CatalogDto cat;
        if (r.status != 304 && r.reachable) {
            cat = models::CatalogDto::fromJson(parseObject(r.body));
        }
        cat.httpStatus = r.status;
        cat.notModified = r.status == 304;
        cat.reachable = r.reachable;
        cat.etag = r.etag;
        cb(cat);
    });
}

} // namespace dish::net
