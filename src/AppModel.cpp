// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "AppModel.h"

#include "LightbarRouting.h"

namespace dish {

AppModel::AppModel(QObject* parent)
    : AppModel(std::make_unique<util::FreedesktopScreenSaverInhibitor>(), parent) {}

AppModel::AppModel(std::unique_ptr<util::DisplaySleepInhibitor> inhibitor, QObject* parent)
    : QObject(parent), store_(std::make_unique<net::ConnectionStore>()),
      wifi_(new net::WifiConnectionManager(store_.get(), this)),
      hub_(new net::ConnectionHub(wifi_, store_.get(), this)),
      bridge_(new input::SDLGamepadBridge(&processor_, this)),
      featureSettings_(new FeatureSettings(this)), autoReconnectTimer_(new QTimer(this)),
      inhibitor_(std::move(inhibitor)), wake_(inhibitor_.get()) {
    QObject::connect(hub_, &net::ConnectionHub::changed, this, &AppModel::onHubChanged);
    QObject::connect(bridge_, &input::SDLGamepadBridge::devicesChanged, this,
                     &AppModel::onBridgeDevicesChanged);
    QObject::connect(wifi_, &net::WifiConnectionManager::connectionEvent, this,
                     &AppModel::onWifiEvent);
    // poolChanged fires every time a WifiConnection is created or transitions
    // state — perfect place to make sure new connections have a rumble
    // handler. Idempotent on already-wired connections.
    QObject::connect(wifi_, &net::WifiConnectionManager::poolChanged, this,
                     &AppModel::installRumbleHandlers);

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
        if (sender) { sender(buttons, lt, rt, lx, ly, rx, ry); }
    });

    processor_.setMotionSender([this](const std::string& did, std::int16_t gx, std::int16_t gy,
                                      std::int16_t gz, std::int16_t ax, std::int16_t ay,
                                      std::int16_t az, std::uint32_t dtUs) {
        net::ConnectionHub::MotionSender sender;
        {
            std::lock_guard<std::mutex> lock(routingMtx_);
            sender = motionRouting_.value(QString::fromStdString(did));
        }
        if (sender) { sender(gx, gy, gz, ax, ay, az, dtUs); }
    });

    processor_.setBatterySender(
        [this](const std::string& did, std::uint8_t level, std::uint8_t status) {
            net::ConnectionHub::BatterySender sender;
            {
                std::lock_guard<std::mutex> lock(routingMtx_);
                sender = batteryRouting_.value(QString::fromStdString(did));
            }
            if (sender) { sender(level, status); }
        });

    processor_.setTouchpadSender(
        [this](const std::string& did, const input::GamepadInputProcessor::TouchpadSample& s) {
            net::ConnectionHub::TouchpadSender sender;
            {
                std::lock_guard<std::mutex> lock(routingMtx_);
                sender = touchpadRouting_.value(QString::fromStdString(did));
            }
            if (sender) {
                sender(s.finger0Active, s.finger0Id, s.finger0X, s.finger0Y, s.finger1Active,
                       s.finger1Id, s.finger1X, s.finger1Y, s.buttonPressed, s.eventTimeMs);
            }
        });

    // Teach the hub how to look up a slot's lightbar capability so a bind()
    // can advertise CAP_LIGHTBAR. The slot id is the SDL bridge device id, so
    // the lookup is a scan of bridge devices for the matching LED flag.
    hub_->setLightbarCapabilityFn([this](const QString& slotId) {
        for (const auto& d : bridge_->devices()) {
            if (d.id == slotId) { return d.hasLightbar; }
        }
        return false;
    });

    // Likewise for motion: bind() advertises CAP_MOTION only when the bound
    // pad has a gyro / accelerometer. Same device-id scan as the lightbar
    // resolver, reading the bridge's per-device IMU probe.
    hub_->setMotionCapabilityFn([this](const QString& slotId) {
        for (const auto& d : bridge_->devices()) {
            if (d.id == slotId) { return d.motionCapable; }
        }
        return false;
    });

    // And the cosmetic controller type: bind() declares the real Xbox-vs-
    // PlayStation kind in MSG_CONTROLLER_TYPE. Same device-id scan, reading
    // the type SDL negotiated for the pad. Defaults to Xbox (0) for a slot
    // with no matching bridge device.
    hub_->setControllerTypeFn([this](const QString& slotId) {
        for (const auto& d : bridge_->devices()) {
            if (d.id == slotId) { return static_cast<int>(d.controllerType); }
        }
        return 0;
    });

    rebuild();
}

AppModel::~AppModel() { bridge_->stop(); }

void AppModel::installRumbleHandlers() {
    for (auto* conn : wifi_->connections()) {
        const QString id = conn->id();
        if (rumbleWiredConnections_.contains(id)) { continue; }
        rumbleWiredConnections_.insert(id);
        // Capture `this` and the connection id by value. The handler runs on
        // the SatelliteClient receive thread; it only reads structures
        // protected by their own locks (hub bindings, bridge device map).
        conn->setRumbleHandler([this, id](const net::SatelliteClient::RumbleMessage& rm) {
            // Find the slot bound to this connection.
            QString deviceId;
            const auto bindings = hub_->bindings();
            for (auto it = bindings.cbegin(); it != bindings.cend(); ++it) {
                if (it.value() == id) {
                    deviceId = it.key();
                    break;
                }
            }
            if (deviceId.isEmpty()) { return; }
            // For dish-linux, slot.id == physical device.id (set in rebuild()),
            // so the slot id IS the bridge's device id. Hand it straight to
            // the SDL bridge. Rumble = vibration only; the light bar has its
            // own return path via MSG_LIGHTBAR.
            bridge_->applyRumble(deviceId, rm.strongMagnitude, rm.weakMagnitude, rm.durationMs);
        });
        // Parallel handler for the dedicated MSG_LIGHTBAR stream (Task 1.4).
        // Resolves the same slot/connection mapping and forwards to the
        // bridge's standalone applyLightbar — independent of rumble. Gated by
        // the light-bar setting: "Off" suppresses the colour entirely.
        conn->setLightbarHandler([this, id](const net::SatelliteClient::LightbarMessage& lm) {
            const auto color =
                lightbarColorFromLightbarMessage(lm, featureSettings_->lightbarFollowGame());
            if (!color) { return; }
            QString deviceId;
            const auto bindings = hub_->bindings();
            for (auto it = bindings.cbegin(); it != bindings.cend(); ++it) {
                if (it.value() == id) {
                    deviceId = it.key();
                    break;
                }
            }
            if (deviceId.isEmpty()) { return; }
            bridge_->applyLightbar(deviceId, color->r, color->g, color->b);
        });
    }
}

void AppModel::start() {
    bridge_->start();
    wifi_->autoReconnectAll();
    autoReconnectTimer_->start();
}

void AppModel::clearPairingTarget() {
    if (!state_.pairingTarget.has_value()) { return; }
    state_.pairingTarget.reset();
    emit stateChanged();
}

void AppModel::onHubChanged() {
    state_.connections = hub_->connections();
    rebuild();
}

void AppModel::onBridgeDevicesChanged() {
    // A new device only matters for routing if a connection is already bound
    // to its slot id, so re-trigger the same rebuild path.
    rebuild();
}

void AppModel::onWifiEvent(const net::ConnectionEvent& evt) {
    switch (evt.kind) {
    case net::ConnectionEventKind::PairingRequired:
        state_.pairingTarget = evt.server;
        emit stateChanged();
        break;
    case net::ConnectionEventKind::Error:
        emit errorMessage(evt.message);
        break;
    }
}

void AppModel::rebuild() {
    QList<models::ControllerSlot> next;
    for (const auto& d : bridge_->devices()) {
        models::ControllerSlot s;
        s.id = d.id;
        s.name = d.name;
        s.physicalDeviceId = d.id;
        s.capabilities.hasMotion = d.motionCapable;
        // Likewise the addressable-LED capability — drives the lightbar chip
        // and tells the hub when to advertise CAP_LIGHTBAR on bind.
        s.capabilities.hasLightbar = d.hasLightbar;
        // Carry the latest battery sample through so the slot card's battery
        // chip can show charge — controller's own for a wireless pad, the
        // host machine's for a wired/unknown one.
        s.capabilities.batteryLevel = d.batteryLevel;
        s.capabilities.batteryStatus = d.batteryStatus;
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
    state_.slotList = std::move(next);

    bool busy = false;
    for (auto* conn : wifi_->connections()) {
        if (conn->isRegisteringController()) {
            busy = true;
            break;
        }
    }
    state_.busy = busy;

    // Update the routing tables to mirror the new slot/binding shape.
    QHash<QString, net::ConnectionHub::ReportSender> nextRouting;
    QHash<QString, net::ConnectionHub::MotionSender> nextMotion;
    QHash<QString, net::ConnectionHub::BatterySender> nextBattery;
    QHash<QString, net::ConnectionHub::TouchpadSender> nextTouchpad;
    for (const auto& slot : state_.slotList) {
        if (auto sender = hub_->reportSenderForSlot(slot.id)) {
            nextRouting.insert(slot.id, std::move(sender));
        }
        if (auto sender = hub_->motionSenderForSlot(slot.id)) {
            nextMotion.insert(slot.id, std::move(sender));
        }
        if (auto sender = hub_->batterySenderForSlot(slot.id)) {
            nextBattery.insert(slot.id, std::move(sender));
        }
        if (auto sender = hub_->touchpadSenderForSlot(slot.id)) {
            nextTouchpad.insert(slot.id, std::move(sender));
        }
    }
    {
        std::lock_guard<std::mutex> lock(routingMtx_);
        routing_ = std::move(nextRouting);
        motionRouting_ = std::move(nextMotion);
        batteryRouting_ = std::move(nextBattery);
        touchpadRouting_ = std::move(nextTouchpad);
    }

    // Drive the display-sleep inhibitor off bindings × hub.connections. The
    // 0↔positive transitions inside ScreenWakeController acquire / release
    // the D-Bus cookie; intermediate same-count emissions are no-ops so a
    // noisy hub feed doesn't thrash the session bus.
    QHash<QString, models::LinkState> connectionStates;
    for (const auto& summary : state_.connections) {
        connectionStates.insert(summary.id, summary.live);
    }
    const int streamingCount =
        util::ScreenWakeController::streamingCount(bindings, connectionStates);
    wake_.update(streamingCount);

    emit stateChanged();
}

} // namespace dish
