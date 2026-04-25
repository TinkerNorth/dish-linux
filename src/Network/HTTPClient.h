// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#pragma once

#include "Models/Models.h"

#include <QObject>
#include <QString>

#include <functional>

class QNetworkAccessManager;

namespace dish::net {

// Thin async wrapper around QNetworkAccessManager for the Satellite connection
// API. Mirrors dish-mac/Network/HTTPClient.swift. The server speaks plain
// HTTP/1.1 on :9877. Callbacks fire on the network manager's home thread.
class HTTPClient : public QObject {
    Q_OBJECT
  public:
    using Callback = std::function<void(const models::ConnectResponse&)>;

    explicit HTTPClient(QObject* parent = nullptr);
    ~HTTPClient() override;

    // POST /api/connections   -> { connectionId, token, ... }
    void connectAsync(const QString& ip, int port, const QString& deviceId, Callback cb);

    // DELETE /api/connections/<id>
    void disconnectAsync(const QString& ip, int port, const QString& connectionId,
                         const QString& deviceId, Callback cb);

  private:
    void perform(const QString& url, const QByteArray& method, const QByteArray& body, Callback cb);

    QNetworkAccessManager* nam_;
};

} // namespace dish::net
