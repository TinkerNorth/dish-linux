// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "HTTPClient.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>

namespace dish::net {

namespace {

constexpr int kTimeoutMs = 5000;

models::ConnectResponse errorResponse(const QString& msg) {
    models::ConnectResponse r;
    r.error = msg;
    return r;
}

} // namespace

HTTPClient::HTTPClient(QObject* parent) : QObject(parent), nam_(new QNetworkAccessManager(this)) {
    nam_->setTransferTimeout(kTimeoutMs);
}

HTTPClient::~HTTPClient() = default;

void HTTPClient::connectAsync(const QString& ip, int port, const QString& deviceId, Callback cb) {
    const QString url = QStringLiteral("http://%1:%2/api/connections").arg(ip).arg(port);
    const auto body =
        QJsonDocument(QJsonObject{{"deviceId", deviceId}}).toJson(QJsonDocument::Compact);
    perform(url, "POST", body, std::move(cb));
}

void HTTPClient::disconnectAsync(const QString& ip, int port, const QString& connectionId,
                                 const QString& deviceId, Callback cb) {
    const QString url =
        QStringLiteral("http://%1:%2/api/connections/%3").arg(ip).arg(port).arg(connectionId);
    const auto body =
        QJsonDocument(QJsonObject{{"deviceId", deviceId}}).toJson(QJsonDocument::Compact);
    perform(url, "DELETE", body, std::move(cb));
}

void HTTPClient::perform(const QString& url, const QByteArray& method, const QByteArray& body,
                         Callback cb) {
    QNetworkRequest req((QUrl(url)));
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    auto* reply = nam_->sendCustomRequest(req, method, body);
    QObject::connect(reply, &QNetworkReply::finished, this, [reply, cb = std::move(cb)] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            cb(errorResponse(reply->errorString()));
            return;
        }
        QJsonParseError err{};
        const auto doc = QJsonDocument::fromJson(reply->readAll(), &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject()) {
            cb(errorResponse(QStringLiteral("malformed")));
            return;
        }
        cb(models::ConnectResponse::fromJson(doc.object()));
    });
}

} // namespace dish::net
