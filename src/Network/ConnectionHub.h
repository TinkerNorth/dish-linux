// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#pragma once

#include "ConnectionStore.h"
#include "Models/Models.h"
#include "WifiConnectionManager.h"

#include <QHash>
#include <QList>
#include <QObject>
#include <QString>

#include <functional>

namespace dish::net {

// Aggregates the wifi pool into the flat [ConnectionSummary] the UI consumes
// and owns the slot->connection binding table. Mirrors
// dish-mac/Network/ConnectionHub.swift (the WiFi-only subset of the Android
// ConnectionHub.kt — no Bluetooth-HID-Device on Linux desktop).
class ConnectionHub : public QObject {
    Q_OBJECT
  public:
    using ReportSender = std::function<void(std::uint16_t, std::uint8_t, std::uint8_t, std::int16_t,
                                            std::int16_t, std::int16_t, std::int16_t)>;

    ConnectionHub(WifiConnectionManager* wifi, ConnectionStore* store, QObject* parent = nullptr);

    QList<models::ConnectionSummary> connections() const { return summaries_; }
    QHash<QString, QString> bindings() const { return bindings_; }

    // Returns the live ReportSender for the connection bound to `slotId`, or
    // an empty function if nothing is bound. The returned closure does a single
    // mutex-guarded shared_ptr load on the hot path; safe to call from the SDL
    // gamepad thread.
    ReportSender reportSenderForSlot(const QString& slotId) const;

    void bind(const QString& slotId, const QString& connectionId);
    void unbind(const QString& slotId);
    std::optional<models::ConnectionSummary> boundConnection(const QString& slotId) const;
    std::optional<models::ConnectionSummary> summary(const QString& id) const;

  signals:
    void changed();

  private:
    void rebuild();

    WifiConnectionManager* wifi_;
    ConnectionStore* store_;
    QList<models::ConnectionSummary> summaries_;
    QHash<QString, QString> bindings_; // slotId -> connectionId
};

} // namespace dish::net
