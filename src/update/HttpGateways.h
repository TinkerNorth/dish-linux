// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The QNetworkAccessManager gateway behind the updater's manifest port. Uses
// Qt's DEFAULT certificate validation (the system root store) and the default
// NoLessSafeRedirectPolicy, which follows a 302 without downgrading to http.
//
// net::HTTPClient / net::PairingClient MUST NOT carry this traffic: they set
// QSslSocket::VerifyNone deliberately (TOFU pinning for satellites on a LAN),
// which is exactly wrong for a public host. That is why this owns a dedicated
// manager rather than borrowing the app's.

#pragma once

#include "update/UpdatePorts.h"

#include <QByteArray>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QString>

#include <memory>

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

namespace dish::update {

// The User-Agent both gateways send: product, version, platform. No device id,
// no account data, nothing else (PRIVACY.md 2.4).
QString updateUserAgent();

class HttpManifestGateway : public QObject, public ManifestGateway {
    Q_OBJECT
  public:
    explicit HttpManifestGateway(QObject* parent = nullptr);
    ~HttpManifestGateway() override;

    void fetch(Callback done) override;
    void cancel() override;

    // Test seam: the permalink is the only URL this ever requests in
    // production, but a fixture server needs to be reachable too.
    void setUrl(const QString& url) { url_ = url; }

  private:
    void finish(const ManifestFetchResult& result);

    QNetworkAccessManager* nam_ = nullptr;
    QPointer<QNetworkReply> reply_;
    Callback done_;
    QString url_;
};

} // namespace dish::update
