// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "update/HttpGateways.h"

#include "core/update/UpdateManifest.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSysInfo>
#include <QTimer>
#include <QUrl>

#include <utility>

#ifndef DISH_VERSION
#define DISH_VERSION "0.0.0"
#endif

namespace dish::update {

namespace {

void applyCommonRequestPolicy(QNetworkRequest& request) {
    request.setRawHeader(QByteArrayLiteral("User-Agent"), updateUserAgent().toUtf8());
    // https-only redirects. Qt 6's default, stated explicitly because the
    // design leans on the release host's 302 being safe.
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                         QNetworkRequest::AlwaysNetwork);
    request.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);
}

QNetworkAccessManager* makeManager(QObject* parent) {
    auto* nam = new QNetworkAccessManager(parent);
    // No cookie jar and no cache: an update check is one anonymous GET, and
    // there is nothing about it worth remembering between runs.
    nam->setCookieJar(nullptr);
    nam->setAutoDeleteReplies(false);
    return nam;
}

} // namespace

// A transport failure that means "this machine cannot reach the internet right
// now" rather than "GitHub said no". The distinction only changes the copy in
// Settings; both back off identically.
reducer::UpdateError classify(QNetworkReply::NetworkError error) {
    switch (error) {
    case QNetworkReply::ConnectionRefusedError:
    case QNetworkReply::HostNotFoundError:
    case QNetworkReply::TemporaryNetworkFailureError:
    case QNetworkReply::NetworkSessionFailedError:
    case QNetworkReply::UnknownNetworkError:
        return reducer::UpdateError::Offline;
    default:
        return reducer::UpdateError::Http;
    }
}

QString updateUserAgent() {
    // Derived, never baked: a hard-coded arch made every non-x86_64 build
    // report x86_64, and the User-Agent is the only thing telling the release
    // host which builds are in the field.
    return QStringLiteral("Dish/%1 (Linux; %2)")
        .arg(QLatin1String(DISH_VERSION), QSysInfo::currentCpuArchitecture());
}

// ── Manifest ────────────────────────────────────────────────────────────────

HttpManifestGateway::HttpManifestGateway(QObject* parent)
    : QObject(parent), nam_(makeManager(this)), url_(QLatin1String(kLatestManifestUrl)) {}

HttpManifestGateway::~HttpManifestGateway() { cancel(); }

void HttpManifestGateway::fetch(Callback done) {
    if (!reply_.isNull()) { return; }
    done_ = std::move(done);

    QNetworkRequest request{QUrl(url_)};
    applyCommonRequestPolicy(request);
    request.setRawHeader(QByteArrayLiteral("Accept"), QByteArrayLiteral("application/json"));
    // The manifest is ~1 KB: 30 s without a byte is a dead link, not a slow one.
    request.setTransferTimeout(30'000);

    QNetworkReply* reply = nam_->get(request);
    reply_ = reply;

    // A captive portal can answer with an arbitrarily large splash page. Cut it
    // off at the cap rather than buffering it just to reject it later.
    QObject::connect(reply, &QNetworkReply::downloadProgress, this,
                     [this](qint64 receivedBytes, qint64) {
                         if (receivedBytes > kManifestMaxBytes && !reply_.isNull()) {
                             reply_->abort();
                         }
                     });
    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        if (reply_ != reply) { return; }
        reply_.clear();
        if (reply->error() != QNetworkReply::NoError) {
            finish(ManifestFetchResult::failed(classify(reply->error())));
            return;
        }
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status != 200) {
            // 404 is the ordinary publish-window case: transient, backed off,
            // self-healing the moment the asset appears.
            finish(ManifestFetchResult::failed(reducer::UpdateError::Http));
            return;
        }
        const QByteArray body = reply->readAll();
        const auto parsed = UpdateManifest::parse(body);
        if (const auto* manifest = std::get_if<UpdateManifest>(&parsed)) {
            finish(ManifestFetchResult::ok(*manifest, body));
            return;
        }
        finish(ManifestFetchResult::failed(reducer::UpdateError::ManifestInvalid));
    });
}

void HttpManifestGateway::cancel() {
    done_ = {};
    if (!reply_.isNull()) {
        QNetworkReply* reply = reply_.data();
        reply_.clear();
        reply->abort();
    }
}

void HttpManifestGateway::finish(const ManifestFetchResult& result) {
    Callback callback;
    callback.swap(done_);
    if (callback) { callback(result); }
}

// ── Payload download ────────────────────────────────────────────────────────

} // namespace dish::update
