// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "AppModel.h"

namespace dish {

AppModel::AppModel(QObject* parent)
    : QObject(parent),
      store_(std::make_unique<net::ConnectionStore>()),
      wifi_(new net::WifiConnectionManager(store_.get(), this)),
      hub_(new net::ConnectionHub(wifi_, store_.get(), this)),
      bridge_(new input::SDLGamepadBridge(&processor_, this)),
      autoReconnectTimer_(new QTimer(this)) {
    QObject::connect(hub_, &net::ConnectionHub::changed, this, &AppModel::onHubChanged);
    QObject::connect(bridge_, &input::SDLGamepadBridge::devicesChanged, this,
                     &AppModel::onBridgeDevicesChanged);
    QObject::connect(wifi_, &net::WifiConnectionManager::event, this, &AppModel::onWifiEvent);

    autoReconnectTimer_->setInterval(15'000);
    QObject::connect(autoReconnectTimer_, &QTimer::timeout, this,
                     [this] { wifi_->autoReconnectAll(); });

    // Hot-path callback. Looks up routing[deviceId] under a short-held mutex
    // and forwards directly. Called on the SDL gamepad thread.
    processor_.setReportSender([this](const std::string& did, std::uint16_t buttons,
                                       std::uint8_t lt, std::uint8_t rt, std::int16_t lx,
                                       std::int16_t ly, std::int16_t rx, std::int16_t ry) {
        net::ConnectionHub::ReportSender sender;
        {
            std::lock_guard<std::mutex> lock(routingMtx_);
            sender = routing_.value(QString::fromStdString(did));
        }
        if (sender) {
            sender(buttons, lt, rt, lx, ly, rx, ry);
        }
    });

    rebuildSlots();
}

AppModel::~AppModel() {
    bridge_->stop();
}

void AppModel::start() {
    bridge_->start();
    wifi_->autoReconnectAll();
    autoReconnectTimer_->start();
}

void AppModel::onHubChanged() {
    connections_ = hub_->connections();
    // Rebuild slots first so the routing table is computed against the
    // currently-attached devices, not a previous snapshot.
    rebuildSlots();
    QHash<QString, net::ConnectionHub::ReportSender> next;
    for (const auto& slot : slots_) {
        auto sender = hub_->reportSenderForSlot(slot.id);
        if (sender) {
            next.insert(slot.id, std::move(sender));
        }
    }
    {
        std::lock_guard<std::mutex> lock(routingMtx_);
        routing_ = std::move(next);
    }
    emit connectionsChanged();
}

void AppModel::onBridgeDevicesChanged() {
    // A new device only matters for routing if a connection is already bound
    // to its slot id, so re-trigger the same rebuild path.
    onHubChanged();
}

void AppModel::onWifiEvent(const net::ConnectionEvent& evt) {
    switch (evt.kind) {
        case net::ConnectionEventKind::PairingRequired:
            pairingTarget_ = evt.server;
            emit pairingTargetChanged();
            break;
        case net::ConnectionEventKind::Error:
            emit errorMessage(evt.message);
            break;
    }
}

void AppModel::rebuildSlots() {
    QList<models::ControllerSlot> next;
    models::ControllerSlot virt;
    virt.id = QString::fromLatin1(models::kVirtualSlotId);
    virt.inputType = models::SlotInputType::Virtual;
    virt.name = QStringLiteral("Virtual Controller");
    next.append(virt);
    for (const auto& d : bridge_->devices()) {
        models::ControllerSlot s;
        s.id = d.id;
        s.inputType = models::SlotInputType::Physical;
        s.name = d.name;
        s.physicalDeviceId = d.id;
        next.append(s);
    }
    // Cross-reference bindings from the hub.
    const auto bindings = hub_->bindings();
    for (auto& s : next) {
        const auto cid = bindings.value(s.id);
        if (!cid.isEmpty()) {
            s.boundConnectionId = cid;
            s.boundStatus = hub_->summary(cid);
        }
    }
    slots_ = std::move(next);
    emit slotsChanged();
}

}  // namespace dish
