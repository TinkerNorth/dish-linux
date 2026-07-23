// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "WifiConnection.h"

#include "Reconcile.h"

#include <cmath>
#include <vector>

namespace dish::net {

namespace {

// Human message for a failed slot converge. Keyed on the protocol apply-result
// string's code (proto::applyResultFromName); mirrors the satellite's
// semantics, not its wording.
QString applyErrorMessage(std::uint8_t resultCode) {
    switch (resultCode) {
    case proto::kApplyNoSlots:
        return QStringLiteral("Server has no free controller slots");
    case proto::kApplyPluginFailed:
        return QStringLiteral("Server failed to plug in the virtual controller");
    case proto::kApplyBackendUnavailable:
        return QStringLiteral(
            "Server has no virtual gamepad backend — controller cannot be created");
    case proto::kApplyInvalidType:
        return QStringLiteral("Server rejected the controller type");
    case proto::kApplyInvalidIndex:
        return QStringLiteral("Server rejected the controller slot index");
    default:
        return QStringLiteral("Server rejected the controller");
    }
}

// Only pads with a physical trackpad route touch as DS4 — the DS4 itself and
// the DualSense. Switch Pro / Xbox have no touch surface. The touchpad-mouse
// host feature is deferred (no UI), so `mouse` is never requested.
std::uint8_t touchpadModeForType(std::uint8_t type) {
    switch (type) {
    case proto::kControllerTypePlayStation:
    case proto::kControllerTypeDualSense:
        return proto::kTouchpadModeDs4;
    default:
        return proto::kTouchpadModeOff;
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
    if (state_ == SessionState::Live || state_ == SessionState::Faltering) { return; }
    state_ = SessionState::Linking;
    emit changed();
}

void WifiConnection::markConnected(const std::shared_ptr<SatelliteClient>& client,
                                   const QString& connectionId, int epoch, SessionHooks hooks) {
    if (state_ != SessionState::Linking) { return; }
    clientRef_.set(client);
    connectionId_ = connectionId;
    state_ = SessionState::Live;
    hooks_ = std::move(hooks);
    lastAppliedEpoch_ = epoch;
    reconcileInFlight_ = false;
    rekeyRequested_ = false;
    latencyOneWayMs_ = 0.0;
    latencySamples_ = 0;

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
    QObject::connect(aliveTimer_, &QTimer::timeout, this, &WifiConnection::onAliveTick);
    aliveTimer_->start();
    emit changed();

    // The session PUT that produced this connection already carried the bound
    // slot's descriptor (the manager marks it applied); this converge only
    // fires when a slot was attached while the session was still linking.
    if (boundSlotId_.has_value() && !controllerAdded_) {
        registerController(pendingControllerType_);
    }
}

void WifiConnection::onAliveTick() {
    const auto c = clientRef_.get();
    if (!c) {
        if (hooks_.onDead) { hooks_.onDead(); }
        return;
    }
    // Close-notify first: an authenticated SESSION_CLOSE is terminal-now — no
    // death wait. The manager maps the reason (unpaired drops the key,
    // replaced stays down, shutdown/kicked re-enter backoff).
    const auto closeReason = c->sessionCloseReason();
    if (closeReason >= 0) {
        if (hooks_.onClose) { hooks_.onClose(static_cast<std::uint8_t>(closeReason)); }
        return;
    }
    if (!c->isAlive()) {
        if (hooks_.onDead) { hooks_.onDead(); }
        return;
    }
    // Live ⇄ Faltering off the consecutive-miss count (contract §Liveness:
    // "not responding" at 2, dead at 5 — death is the branch above).
    const bool faltering = c->missedAcks() >= SatelliteClient::kHeartbeatMissNotResponding;
    const SessionState want = faltering ? SessionState::Faltering : SessionState::Live;
    if (state_ != want) {
        state_ = want;
        emit changed();
    }
    // Latency readout: cache the snapshot rounded to 0.1 ms so the signal only
    // fires when the displayed value moves. telemetryChanged, not changed —
    // a cosmetic 1 Hz tick must not run the wholesale UI rebuild.
    const auto snap = c->latencySnapshot();
    const double rounded = std::round(snap.oneWayMs * 10.0) / 10.0;
    if (rounded != latencyOneWayMs_ || snap.samples != latencySamples_) {
        latencyOneWayMs_ = rounded;
        latencySamples_ = snap.samples;
        emit telemetryChanged();
    }
    // Reconcile: the enriched ack's epoch/bitmap vs what we last applied.
    // Single-flight — the manager clears the guard when its GET lands.
    if (!reconcileInFlight_ && hooks_.reconcile) {
        std::vector<reducer::DesiredSlot> desired;
        if (controllerAdded_) {
            desired.push_back({static_cast<std::uint8_t>(kDefaultCtrlIndex),
                               static_cast<std::uint8_t>(pendingControllerType_)});
        }
        if (reducer::reconcileNeeded(c->serverEpoch(), c->serverBitmap(), lastAppliedEpoch_,
                                     reducer::expectedBitmap(desired))) {
            hooks_.reconcile();
        }
    }
    // Proactive re-key before the send counter can exhaust (contract §Crypto:
    // re-PUT past 0xF0000000). A session that exhausts anyway goes silent in
    // SatelliteClient and heals via the death-retry re-PUT.
    if (reducer::counterNeedsRepush(c->sendCounter())) {
        if (!rekeyRequested_ && hooks_.rekey) {
            rekeyRequested_ = true;
            hooks_.rekey();
        }
    } else {
        rekeyRequested_ = false;
    }
}

void WifiConnection::markDisconnected() {
    auto existing = clientRef_.get();
    if (state_ == SessionState::Idle && !existing) { return; }
    if (aliveTimer_ != nullptr) {
        aliveTimer_->stop();
        aliveTimer_->deleteLater();
        aliveTimer_ = nullptr;
    }
    controllerRegistering_ = false;
    if (existing) {
        existing->stopHeartbeat();
        existing->stopReceiveLoop();
        existing->closeSocket();
    }
    clientRef_.set(nullptr);
    connectionId_.reset();
    controllerAdded_ = false;
    lastAppliedEpoch_ = -1;
    reconcileInFlight_ = false;
    rekeyRequested_ = false;
    latencyOneWayMs_ = 0.0;
    latencySamples_ = 0;
    hooks_ = {};
    state_ = SessionState::Idle;
    emit changed();
}

void WifiConnection::attachSlot(const QString& slotId, int controllerType, bool hasLightbar,
                                bool hasMotion) {
    boundSlotId_ = slotId;
    pendingControllerType_ = controllerType;
    lightbarCapable_ = hasLightbar;
    motionCapable_ = hasMotion;
    if ((state_ == SessionState::Live || state_ == SessionState::Faltering) && !controllerAdded_) {
        registerController(controllerType);
    }
    emit changed();
}

void WifiConnection::detachSlot() {
    if (!boundSlotId_.has_value()) { return; }
    boundSlotId_.reset();
    if (controllerAdded_ && hooks_.deleteSlot) { hooks_.deleteSlot(kDefaultCtrlIndex); }
    controllerAdded_ = false;
    emit changed();
}

std::optional<models::ControllerDescriptor> WifiConnection::desiredDescriptor() const {
    if (!boundSlotId_.has_value()) { return std::nullopt; }
    models::ControllerDescriptor desc;
    desc.ctrlIdx = kDefaultCtrlIndex;
    desc.type = static_cast<std::uint8_t>(pendingControllerType_);
    desc.caps = SatelliteClient::withLightbarCapability(
        SatelliteClient::withMotionCapability(kDefaultCaps, motionCapable_), lightbarCapable_);
    desc.touchpadMode = touchpadModeForType(desc.type);
    return desc;
}

void WifiConnection::markSlotApplied() {
    if (boundSlotId_.has_value()) { controllerAdded_ = true; }
}

void WifiConnection::registerController(int type) {
    pendingControllerType_ = type;
    if (!hooks_.putSlot) { return; }
    const auto desc = desiredDescriptor();
    if (!desc.has_value()) { return; }
    controllerRegistering_ = true;
    emit changed();
    hooks_.putSlot(*desc, [this](const models::ControllerPutResponse& resp) {
        controllerRegistering_ = false;
        const auto slotId = boundSlotId_.value_or(QString());
        // Terminal 401s were already consumed by the manager's wrapper (key
        // dropped, session torn) — nothing to converge here.
        if (resp.unauthorized()) {
            emit changed();
            return;
        }
        if (!resp.reachable || !resp.controller.has_value()) {
            emit changed();
            emit errorOccurred(QStringLiteral("Server did not acknowledge the controller"));
            if (!slotId.isEmpty()) { emit registrationFailed(slotId); }
            return;
        }
        // replugFailed still counts as live: the PREVIOUS pad stays plugged
        // and streams keep flowing (appliedType reports what's in force).
        if (resp.controller->slotIsLive()) {
            controllerAdded_ = true;
            setLastAppliedEpoch(resp.epoch);
            emit changed();
            return;
        }
        emit changed();
        emit errorOccurred(applyErrorMessage(resp.controller->resultCode));
        if (!slotId.isEmpty()) { emit registrationFailed(slotId); }
    });
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
                                  std::int16_t finger1X, std::int16_t finger1Y, bool buttonPressed,
                                  std::uint32_t eventTimeMs) {
    if (auto c = clientRef_.get()) {
        c->sendTouchpad(kDefaultCtrlIndex, finger0Active, finger0Id, finger0X, finger0Y,
                        finger1Active, finger1Id, finger1X, finger1Y, buttonPressed, eventTimeMs);
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
