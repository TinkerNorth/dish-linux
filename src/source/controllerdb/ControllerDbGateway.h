// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The controller database's one network seam: a GET of one file under
// https://dish.tinkernorth.com/controllers/v1/, with If-None-Match when the
// caller has an ETag. Abstract so the check's flow and its switch are tested
// against a fake that counts requests (the update gateways' pattern).
//
// The HTTP implementation owns a QNetworkAccessManager of its own with Qt's
// default certificate validation, no cookie jar and no cache, and sends no
// query, no identifier and Qt's default User-Agent (section 6.8), never the
// update check's product string. net::HTTPClient must not carry it: that one
// skips certificate validation for LAN satellites.

#pragma once

#include <QByteArray>
#include <QObject>
#include <QPointer>
#include <QString>

#include <functional>

class QNetworkAccessManager;
class QNetworkReply;

namespace dish::source::controllerdb {

struct DbReply {
    bool transportFailed = false;
    int status = 0;
    QByteArray body;
    QByteArray etag;
};

class ControllerDbPort {
  public:
    using Callback = std::function<void(const DbReply&)>;

    virtual ~ControllerDbPort() = default;

    // `path` is relative to the database's base URL. One callback per accepted get; a get while
    // one is in flight is refused by the caller's own sequencing, never queued here.
    virtual void get(const QString& path, const QByteArray& ifNoneMatch, Callback done) = 0;

    // Drops the request in flight; its callback is never delivered.
    virtual void cancel() = 0;
};

class HttpControllerDbGateway : public QObject, public ControllerDbPort {
    Q_OBJECT
  public:
    explicit HttpControllerDbGateway(QObject* parent = nullptr);
    ~HttpControllerDbGateway() override;

    void get(const QString& path, const QByteArray& ifNoneMatch, Callback done) override;
    void cancel() override;

  private:
    void onFinished(QNetworkReply* reply);
    void onProgress(qint64 received, qint64 total);

    QNetworkAccessManager* nam_ = nullptr;
    QPointer<QNetworkReply> reply_;
    Callback done_;
    qint64 cap_ = 0;
    bool tooLarge_ = false;
};

} // namespace dish::source::controllerdb
