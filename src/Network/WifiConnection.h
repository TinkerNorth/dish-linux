// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#pragma once

#include "Models/Models.h"
#include "SatelliteClient.h"

#include <QObject>
#include <QString>
#include <QTimer>

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>

namespace dish::net {

enum class WifiState { Idle, Connecting, Connected };

// Thread-safe holder for the live SatelliteClient pointer. Writes from the Qt
// main thread (markConnected/markDisconnected); reads from the SDL gamepad
// thread on every report. Guarded by std::mutex.
class ClientRef {
  public:
    std::shared_ptr<SatelliteClient> get() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return value_;
    }
    void set(std::shared_ptr<SatelliteClient> v) {
        std::lock_guard<std::mutex> lock(mtx_);
        value_ = std::move(v);
    }

  private:
    mutable std::mutex mtx_;
    std::shared_ptr<SatelliteClient> value_;
};

// A single live or potential WiFi session to one Satellite server. Mirrors
// dish-mac/Network/WifiConnection.swift.
class WifiConnection : public QObject {
    Q_OBJECT
  public:
    static QString idFor(const models::DiscoveredServer& s) { return s.id(); }

    WifiConnection(QString id, models::DiscoveredServer server, QObject* parent = nullptr);
    ~WifiConnection() override;

    QString id() const { return id_; }
    const models::DiscoveredServer& server() const { return server_; }
    WifiState state() const { return state_; }
    std::optional<QString> connectionId() const { return connectionId_; }
    std::optional<QString> boundSlotId() const { return boundSlotId_; }
    std::shared_ptr<SatelliteClient> client() const { return clientRef_.get(); }

    void updateServer(const models::DiscoveredServer& s);
    void markConnecting();
    void markConnected(std::shared_ptr<SatelliteClient> client, const QString& connectionId,
                       std::function<void()> onDead);
    void markDisconnected();

    void attachSlot(const QString& slotId, int controllerType);
    void detachSlot();

    bool isRegisteringController() const { return controllerRegistering_; }

    // Hot path: called directly from the SDL gamepad thread.
    void sendReport(std::uint16_t buttons, std::uint8_t lt, std::uint8_t rt, std::int16_t lx,
                    std::int16_t ly, std::int16_t rx, std::int16_t ry);

    // Install the per-connection rumble handler. The handler is invoked from
    // the SatelliteClient's receive thread on every MSG_RUMBLE we decode.
    // Stored on the WifiConnection (not the per-session SatelliteClient) so
    // it survives reconnects: markConnected() re-installs it on the new
    // client instance.
    using RumbleHandler = std::function<void(const SatelliteClient::RumbleMessage&)>;
    void setRumbleHandler(RumbleHandler handler);

    using LightbarHandler = std::function<void(const SatelliteClient::LightbarMessage&)>;
    void setLightbarHandler(LightbarHandler handler);

  signals:
    void changed();
    void errorOccurred(const QString& message);
    // Emitted when the in-flight controller registration for `slotId` was
    // rejected or timed out. Listened to by ConnectionHub to roll back the
    // local binding so the UI reflects reality.
    void registrationFailed(const QString& slotId);

  private:
    static constexpr int kDefaultCtrlIndex = 0;
    static constexpr std::uint16_t kDefaultCaps = 0x0003;
    static constexpr int kAckWaitAttempts = 20;
    static constexpr int kAckWaitIntervalMs = 100;

    void registerController(int type);
    void pollControllerAck();
    void finishRegistration();

    QString id_;
    models::DiscoveredServer server_;
    WifiState state_ = WifiState::Idle;
    std::optional<QString> connectionId_;
    std::optional<QString> boundSlotId_;

    ClientRef clientRef_;
    QTimer* aliveTimer_ = nullptr;
    QTimer* ackPollTimer_ = nullptr;
    int ackPollCount_ = 0;
    bool controllerRegistering_ = false;
    std::function<void()> onDead_;
    bool controllerAdded_ = false;
    int pendingControllerType_ = 0;

    // Set once during composition; re-applied to each fresh SatelliteClient
    // in markConnected() so we don't lose rumble across reconnects.
    RumbleHandler rumbleHandler_;
    LightbarHandler lightbarHandler_;
};

} // namespace dish::net
