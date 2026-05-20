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
    if (state_ == SessionState::Live) { return; }
    state_ = SessionState::Linking;
    emit changed();
}

void WifiConnection::markConnected(std::shared_ptr<SatelliteClient> client,
                                   const QString& connectionId, std::function<void()> onDead) {
    if (state_ != SessionState::Linking) { return; }
    clientRef_.set(client);
    connectionId_ = connectionId;
    state_ = SessionState::Live;
    onDead_ = std::move(onDead);

    client->resetControllerAck();
    if (rumbleHandler_) { client->setRumbleHandler(rumbleHandler_); }
    if (lightbarHandler_) { client->setLightbarHandler(lightbarHandler_); }
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
    // TODO(SessionState::Faltering / LinkState::Unstable): when the native
    // alive-poll exposes the consecutive-missed-heartbeat count, this is the
    // transition point that should flip Live → Faltering on a non-zero miss
    // count (and only collapse to Idle when misses hit the death threshold).
    // Today the alive-poll's onDead_() callback runs disconnect() directly,
    // so Faltering / Unstable are defined but never entered.
    if (state_ == SessionState::Idle && !existing) { return; }
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
    state_ = SessionState::Idle;
    emit changed();
}

void WifiConnection::attachSlot(const QString& slotId, int controllerType, bool hasLightbar,
                                bool hasMotion) {
    boundSlotId_ = slotId;
    pendingControllerType_ = controllerType;
    lightbarCapable_ = hasLightbar;
    motionCapable_ = hasMotion;
    if (state_ == SessionState::Live && !controllerAdded_) { registerController(controllerType); }
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
    // Per-controller capability word: the static base (analog triggers,
    // rumble) plus CAP_MOTION only when the bound pad has a gyro / accel and
    // CAP_LIGHTBAR only when it has an addressable RGB LED. Mirrors the spec's
    //   caps = base | (hasMotion ? 0x0004 : 0) | (hasLed ? 0x0008 : 0)
    const std::uint16_t caps = SatelliteClient::withLightbarCapability(
        SatelliteClient::withMotionCapability(kDefaultCaps, motionCapable_), lightbarCapable_);
    c->controllerAdd(kDefaultCtrlIndex, caps);
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

void WifiConnection::sendMotion(std::int16_t gyroX, std::int16_t gyroY, std::int16_t gyroZ,
                                std::int16_t accelX, std::int16_t accelY, std::int16_t accelZ,
                                std::uint32_t timestampDeltaUs) {
    if (auto c = clientRef_.get()) {
        c->sendMotion(kDefaultCtrlIndex, gyroX, gyroY, gyroZ, accelX, accelY, accelZ,
                      timestampDeltaUs);
    }
}

void WifiConnection::sendBattery(std::uint8_t level, std::uint8_t status) {
    if (auto c = clientRef_.get()) { c->sendBattery(kDefaultCtrlIndex, level, status); }
}

void WifiConnection::sendTouchpad(bool finger0Active, std::uint8_t finger0Id, std::int16_t finger0X,
                                  std::int16_t finger0Y, bool finger1Active, std::uint8_t finger1Id,
                                  std::int16_t finger1X, std::int16_t finger1Y,
                                  bool buttonPressed) {
    if (auto c = clientRef_.get()) {
        c->sendTouchpad(kDefaultCtrlIndex, finger0Active, finger0Id, finger0X, finger0Y,
                        finger1Active, finger1Id, finger1X, finger1Y, buttonPressed);
    }
}

void WifiConnection::setRumbleHandler(RumbleHandler handler) {
    rumbleHandler_ = std::move(handler);
    // Apply immediately if a session is already live; otherwise markConnected
    // will pick up the new handler the next time it runs.
    if (auto c = clientRef_.get()) { c->setRumbleHandler(rumbleHandler_); }
}

void WifiConnection::setLightbarHandler(LightbarHandler handler) {
    lightbarHandler_ = std::move(handler);
    if (auto c = clientRef_.get()) { c->setLightbarHandler(lightbarHandler_); }
}

} // namespace dish::net
