// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#pragma once

#include "Input/GamepadInputProcessor.h"
#include "Input/SDLGamepadBridge.h"
#include "Models/Models.h"
#include "Network/ConnectionHub.h"
#include "Network/ConnectionStore.h"
#include "Network/WifiConnectionManager.h"

#include <QHash>
#include <QObject>
#include <QString>
#include <QTimer>

#include <memory>
#include <mutex>

namespace dish {

// Top-level application state. Owns the network + input layers and stitches
// them together the same way the Mac AppModel and Android MainViewModel do.
//
//   * exposes a flat slot list (1 virtual + 1 per attached SDL gamepad),
//   * maintains a slotId -> WifiConnection routing table updated from the Qt
//     main thread and consulted from the SDL gamepad thread on every report,
//   * surfaces a transient error banner + a pairing-required signal.
class AppModel : public QObject {
    Q_OBJECT
public:
    explicit AppModel(QObject* parent = nullptr);
    ~AppModel() override;

    net::ConnectionStore* store() { return store_.get(); }
    net::WifiConnectionManager* wifi() { return wifi_; }
    net::ConnectionHub* hub() { return hub_; }
    input::GamepadInputProcessor* processor() { return &processor_; }
    input::SDLGamepadBridge* bridge() { return bridge_; }

    QList<models::ControllerSlot> slotList() const { return slots_; }
    QList<models::ConnectionSummary> connections() const { return connections_; }
    std::optional<models::DiscoveredServer> pairingTarget() const { return pairingTarget_; }

    void clearPairingTarget() { pairingTarget_.reset(); }

    void start();

signals:
    void slotsChanged();
    void connectionsChanged();
    void pairingTargetChanged();
    void errorMessage(const QString& msg);

private:
    void rebuildSlots();
    void onHubChanged();
    void onBridgeDevicesChanged();
    void onWifiEvent(const net::ConnectionEvent& evt);

    std::unique_ptr<net::ConnectionStore> store_;
    net::WifiConnectionManager* wifi_;
    net::ConnectionHub* hub_;
    input::GamepadInputProcessor processor_;
    input::SDLGamepadBridge* bridge_;
    QTimer* autoReconnectTimer_;

    QList<models::ControllerSlot> slots_;
    QList<models::ConnectionSummary> connections_;
    std::optional<models::DiscoveredServer> pairingTarget_;

    // slotId -> active sender. Read on the SDL gamepad thread; written on the
    // Qt main thread. Guarded by routingMtx_ for both directions.
    mutable std::mutex routingMtx_;
    QHash<QString, net::ConnectionHub::ReportSender> routing_;
};

}  // namespace dish
