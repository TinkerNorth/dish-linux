// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "source/controllerdb/ControllerDbGateway.h"

#include "core/controllerdb/DbFetch.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

#include <utility>

namespace dish::source::controllerdb {

namespace {

namespace cdb = dish::controllerdb;

// The manifest is a few hundred bytes and the records a few hundred kilobytes: half a minute
// without a byte is a dead link.
constexpr int kTransferTimeoutMs = 30'000;
// What a body cut off at the cap is reported as: the server's answer was wrong, not absent.
constexpr int kPayloadTooLarge = 413;

QUrl urlFor(const QString& path) {
    const QString base =
        QString::fromLatin1(cdb::kDbBaseUrl.data(), static_cast<qsizetype>(cdb::kDbBaseUrl.size()));
    return {base + path};
}

qint64 capFor(const QString& path) {
    const bool isManifest =
        path ==
        QLatin1String(cdb::kManifestPath.data(), static_cast<qsizetype>(cdb::kManifestPath.size()));
    return isManifest ? cdb::kManifestMaxBytes : cdb::kFileMaxBytes;
}

} // namespace

HttpControllerDbGateway::HttpControllerDbGateway(QObject* parent)
    : QObject(parent), nam_(new QNetworkAccessManager(this)) {
    nam_->setCookieJar(nullptr);
    nam_->setAutoDeleteReplies(false);
}

HttpControllerDbGateway::~HttpControllerDbGateway() { cancel(); }

void HttpControllerDbGateway::get(const QString& path, const QByteArray& ifNoneMatch,
                                  Callback done) {
    if (!reply_.isNull()) { return; }
    done_ = std::move(done);
    cap_ = capFor(path);
    tooLarge_ = false;

    QNetworkRequest request{urlFor(path)};
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                         QNetworkRequest::AlwaysNetwork);
    request.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);
    request.setTransferTimeout(kTransferTimeoutMs);
    if (!ifNoneMatch.isEmpty()) {
        request.setRawHeader(QByteArrayLiteral("If-None-Match"), ifNoneMatch);
    }

    QNetworkReply* reply = nam_->get(request);
    reply_ = reply;
    QObject::connect(reply, &QNetworkReply::downloadProgress, this,
                     &HttpControllerDbGateway::onProgress);
    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply] { onFinished(reply); });
}

// Cut off at the cap rather than buffered and refused afterwards.
void HttpControllerDbGateway::onProgress(qint64 received, qint64 /*total*/) {
    if (received > cap_ && !reply_.isNull()) {
        tooLarge_ = true;
        reply_->abort();
    }
}

void HttpControllerDbGateway::onFinished(QNetworkReply* reply) {
    reply->deleteLater();
    if (reply_ != reply) { return; }
    reply_.clear();

    DbReply out;
    out.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    // A 304 arrives with NoError; a reply with no status at all never reached the server.
    out.transportFailed = reply->error() != QNetworkReply::NoError && out.status == 0;
    out.body = reply->readAll();
    out.etag = reply->rawHeader(QByteArrayLiteral("ETag"));
    if (tooLarge_) { out = DbReply{false, kPayloadTooLarge, {}, {}}; }

    Callback callback;
    callback.swap(done_);
    if (callback) { callback(out); }
}

void HttpControllerDbGateway::cancel() {
    done_ = {};
    if (!reply_.isNull()) {
        QNetworkReply* reply = reply_.data();
        reply_.clear();
        reply->abort();
    }
}

} // namespace dish::source::controllerdb
