// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "WifiConnection.h"

namespace dish::net {

namespace {

QString controllerAckErrorMessage(std::uint8_t result) {
    // Matches the satellite/src/core/types.h codes verbatim.
    switch (result) {
    case 0x01:
        return QStringLiteral(
            "Server has no virtual gamepad backend — controller cannot be created");
    case 0x02:
        return QStringLiteral("Server has no free controller slots");
    case 0x03:
        return QStringLiteral("Controller already added on the server");
    case 0x04:
        return QStringLiteral("Controller not found on the server");
    case 0x05:
        return QStringLiteral("Server failed to plug in the virtual controller");
    default:
        return QStringLiteral("Server rejected controller add (code %1)").arg(result);
    }
}

} // namespace

WifiConnection::WifiConnection(QString id, models::DiscoveredServer server, QObject* parent)
    : QObject(parent), id_(std::move(id)), server_(std::move(server)) {}

WifiConnection::~WifiConnection() { markDisconnected(); }

void WifiConnection::updateServer(const models::DiscoveredServer& s) {
    server_ = s;
    emit changed();
}

void WifiConnection::markConnecting() {
    if (state_ == WifiState::Connected) { return; }
    state_ = WifiState::Connecting;
    emit changed();
}

void WifiConnection::markConnected(std::shared_ptr<SatelliteClient> client,
                                   const QString& connectionId, std::function<void()> onDead) {
    if (state_ != WifiState::Connecting) { return; }
    clientRef_.set(client);
    connectionId_ = connectionId;
    state_ = WifiState::Connected;
    onDead_ = std::move(onDead);

    client->resetControllerAck();
    client->startReceiveLoop();
    client->startHeartbeat();

    if (aliveTimer_ != nullptr) {
        aliveTimer_->stop();
        aliveTimer_->deleteLater();
    }
    aliveTimer_ = new QTimer(this);
    aliveTimer_->setInterval(1000);
    QObject::connect(aliveTimer_, &QTimer::timeout, this, [this] {
        const auto c = clientRef_.get();
        if (!c || !c->isAlive()) {
            const auto cb = onDead_;
            if (cb) { cb(); }
        }
    });
    aliveTimer_->start();
    emit changed();

    if (boundSlotId_.has_value() && !controllerAdded_) {
        registerController(pendingControllerType_);
    }
}

void WifiConnection::markDisconnected() {
    auto existing = clientRef_.get();
    if (state_ == WifiState::Idle && !existing) { return; }
    if (aliveTimer_ != nullptr) {
        aliveTimer_->stop();
        aliveTimer_->deleteLater();
        aliveTimer_ = nullptr;
    }
    if (ackPollTimer_ != nullptr) { ackPollTimer_->stop(); }
    controllerRegistering_ = false;
    if (existing) {
        existing->stopHeartbeat();
        existing->stopReceiveLoop();
        existing->closeSocket();
    }
    clientRef_.set(nullptr);
    connectionId_.reset();
    controllerAdded_ = false;
    state_ = WifiState::Idle;
    emit changed();
}

void WifiConnection::attachSlot(const QString& slotId, int controllerType) {
    boundSlotId_ = slotId;
    pendingControllerType_ = controllerType;
    if (state_ == WifiState::Connected && !controllerAdded_) { registerController(controllerType); }
    emit changed();
}

void WifiConnection::detachSlot() {
    if (!boundSlotId_.has_value()) { return; }
    boundSlotId_.reset();
    if (controllerAdded_) {
        if (auto c = clientRef_.get()) { c->controllerRemove(kDefaultCtrlIndex); }
    }
    controllerAdded_ = false;
    emit changed();
}

void WifiConnection::registerController(int type) {
    auto c = clientRef_.get();
    if (!c) { return; }
    pendingControllerType_ = type;
    c->resetControllerAck();
    c->controllerAdd(kDefaultCtrlIndex, kDefaultCaps);
    ackPollCount_ = 0;
    controllerRegistering_ = true;
    if (ackPollTimer_ == nullptr) {
        ackPollTimer_ = new QTimer(this);
        ackPollTimer_->setInterval(kAckWaitIntervalMs);
        QObject::connect(ackPollTimer_, &QTimer::timeout, this, &WifiConnection::pollControllerAck);
    }
    ackPollTimer_->start();
    emit changed();
}

void WifiConnection::pollControllerAck() {
    auto c = clientRef_.get();
    if (!c) {
        const auto slotId = boundSlotId_.value_or(QString());
        finishRegistration();
        emit errorOccurred(QStringLiteral("Connection dropped before controller acknowledgement"));
        if (!slotId.isEmpty()) { emit registrationFailed(slotId); }
        return;
    }
    const auto ack = c->lastControllerAck();
    if (ack == -1) {
        if (++ackPollCount_ >= kAckWaitAttempts) {
            const auto slotId = boundSlotId_.value_or(QString());
            finishRegistration();
            emit errorOccurred(
                QStringLiteral("Server did not acknowledge controller add (timeout)"));
            if (!slotId.isEmpty()) { emit registrationFailed(slotId); }
        }
        return;
    }
    const std::uint8_t result = static_cast<std::uint8_t>(ack & 0xFF);
    if (result == 0x00 /* ACK_OK */) {
        c->sendControllerType(kDefaultCtrlIndex, pendingControllerType_);
        controllerAdded_ = true;
        finishRegistration();
    } else {
        const auto slotId = boundSlotId_.value_or(QString());
        finishRegistration();
        emit errorOccurred(controllerAckErrorMessage(result));
        if (!slotId.isEmpty()) { emit registrationFailed(slotId); }
    }
}

void WifiConnection::finishRegistration() {
    if (ackPollTimer_ != nullptr) { ackPollTimer_->stop(); }
    controllerRegistering_ = false;
    emit changed();
}

void WifiConnection::sendReport(std::uint16_t buttons, std::uint8_t lt, std::uint8_t rt,
                                std::int16_t lx, std::int16_t ly, std::int16_t rx,
                                std::int16_t ry) {
    if (auto c = clientRef_.get()) {
        c->sendReport(kDefaultCtrlIndex, buttons, lt, rt, lx, ly, rx, ry);
    }
}

} // namespace dish::net
