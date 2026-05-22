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

// Internal wire-level session state for one Satellite connection. This is the
// "Presence" axis (per the shared nomenclature): how far the live network link
// has progressed for *this* connection.
//
// Distinct from the UI-facing [models::LinkState] (in Models.h), which folds
// pairing/discovery in on top of this.
//
// - Idle       — no live session (paired or not).
// - Linking    — pair+auth handshake / markConnecting is in flight; native
//                socket not yet open. UI chip: "Connecting…".
// - Live       — native socket open, heartbeat ACKs flowing. UI chip: "Online".
// - Faltering  — Live, but the heartbeat-miss counter is non-zero and below
//                the death threshold. UI chip: "Unsteady". **Not yet entered**
//                — reaching it requires the native side to expose the
//                consecutive-missed count separately from the binary
//                isAlive() boolean. Today the alive-poll flips Live → Idle
//                directly when misses hit the threshold.
enum class SessionState { Idle, Linking, Live, Faltering };

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
    SessionState state() const { return state_; }
    std::optional<QString> connectionId() const { return connectionId_; }
    std::optional<QString> boundSlotId() const { return boundSlotId_; }
    std::shared_ptr<SatelliteClient> client() const { return clientRef_.get(); }

    void updateServer(const models::DiscoveredServer& s);
    void markConnecting();
    void markConnected(std::shared_ptr<SatelliteClient> client, const QString& connectionId,
                       std::function<void()> onDead);
    void markDisconnected();

    // Bind this connection to a controller slot. `controllerType` is the
    // satellite virtual-device type (CONTROLLER_TYPE_*). `hasLightbar` is true
    // when the bound physical pad exposes an addressable RGB LED — it gates
    // the CAP_LIGHTBAR (0x0008) bit in the MSG_CONTROLLER_ADD capability word.
    // `hasMotion` is true when the pad has a gyro / accelerometer — it gates
    // the CAP_MOTION (0x0004) bit the same way. Both are stored so a later
    // registration (on reconnect) advertises the same capabilities.
    void attachSlot(const QString& slotId, int controllerType, bool hasLightbar, bool hasMotion);
    void detachSlot();

    bool isRegisteringController() const { return controllerRegistering_; }

    // Push a fresh CAP_MOTION / CAP_LIGHTBAR / base capability word for the
    // already-registered controller without an unplug. Wired but currently
    // unused at runtime: dish-linux has no per-slot motion toggle UI, so the
    // capability word is fixed at registerController() time. The helper exists
    // so a future per-controller `motionEnabled` toggle (CLI flag, settings
    // file, or tray menu) can land without re-touching the wire layer — the
    // dish-android pattern. No-op when there is no live client or the
    // controller is not yet registered.
    //
    // TODO(motion-toggle-ui): when dish-linux grows a runtime motion-enabled
    // toggle, recompute the caps word here (mirroring registerController) and
    // call this on every transition. A future PR.
    void sendCapsUpdate(std::uint16_t capabilities);

    // Hot path: called directly from the SDL gamepad thread.
    void sendReport(std::uint16_t buttons, std::uint8_t lt, std::uint8_t rt, std::int16_t lx,
                    std::int16_t ly, std::int16_t rx, std::int16_t ry);

    // Hot path: forward an IMU sample to the satellite. Called from the
    // GamepadInputProcessor's motion publish path on the SDL sensor thread.
    void sendMotion(std::int16_t gyroX, std::int16_t gyroY, std::int16_t gyroZ, std::int16_t accelX,
                    std::int16_t accelY, std::int16_t accelZ, std::uint32_t timestampDeltaUs);

    // Forward a battery sample to the satellite. Called from the battery-poll
    // path on the SDL gamepad thread (30 s default cadence).
    void sendBattery(std::uint8_t level, std::uint8_t status);

    // Forward a touchpad sample to the satellite. Called from the SDL
    // touchpad-event path. Up to two fingers + the clickable-pad button.
    void sendTouchpad(bool finger0Active, std::uint8_t finger0Id, std::int16_t finger0X,
                      std::int16_t finger0Y, bool finger1Active, std::uint8_t finger1Id,
                      std::int16_t finger1X, std::int16_t finger1Y, bool buttonPressed);

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
    // Emitted once per successful registration when the satellite ACKed our
    // CAP_MOTION advertisement with a backend status that disagrees. Carries a
    // short, user-facing reason for the toast / log surface; severity is
    // informational rather than an error (motion still won't work, but
    // gameplay does). A pre-extension satellite never triggers this — the
    // motion-flags optional is std::nullopt in that case and we stay quiet.
    // Mirrors dish-android's SatelliteMotionBackendStatusStore handling.
    void motionBackendStatus(const QString& message);

  private:
    static constexpr int kDefaultCtrlIndex = 0;
    // Base capability word advertised in MSG_CONTROLLER_ADD: analog triggers
    // (0x0001) | rumble (0x0002). Neither CAP_MOTION (0x0004) nor CAP_LIGHTBAR
    // (0x0008) is in here — both are per-controller (CAP_MOTION only for pads
    // with an IMU, CAP_LIGHTBAR only for pads with an LED) and are OR-ed in by
    // registerController from motionCapable_ / lightbarCapable_. See
    // SatelliteClient::kCap* mirrors.
    static constexpr std::uint16_t kDefaultCaps =
        SatelliteClient::kCapAnalogTriggers | SatelliteClient::kCapRumble;
    static constexpr int kAckWaitAttempts = 20;
    static constexpr int kAckWaitIntervalMs = 100;

    void registerController(int type);
    void pollControllerAck();
    void finishRegistration();

    QString id_;
    models::DiscoveredServer server_;
    SessionState state_ = SessionState::Idle;
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
    // Whether the bound slot's physical pad has an addressable RGB LED. Set by
    // attachSlot; consumed by registerController to advertise CAP_LIGHTBAR.
    bool lightbarCapable_ = false;
    // Whether the bound slot's physical pad has a gyro / accelerometer. Set by
    // attachSlot; consumed by registerController to advertise CAP_MOTION.
    bool motionCapable_ = false;

    // Set once during composition; re-applied to each fresh SatelliteClient
    // in markConnected() so we don't lose rumble across reconnects.
    RumbleHandler rumbleHandler_;
    LightbarHandler lightbarHandler_;
};

} // namespace dish::net
