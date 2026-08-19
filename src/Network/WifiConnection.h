// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#pragma once

#include "Models/Models.h"
#include "SatelliteClient.h"

#include <QObject>
#include <QString>
#include <QTimer>

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace dish::net {

// Wire-level session state, distinct from the UI-facing models::LinkState, which
// folds pairing and discovery in on top of this.
//
// - Idle       — no live session, paired or not.
// - Linking    — session PUT / auth handshake in flight.
// - Live       — UDP socket open, heartbeat acks flowing.
// - Faltering  — live but past the miss threshold and short of the death one.
// - Stale      — collapsed with the key dropped by a terminal 401 or a
//                close-notify(unpaired), or a silent retry is in flight.
enum class SessionState { Idle, Linking, Live, Faltering, Stale };

// Written on the Qt main thread, read on the SDL input thread every report.
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

// One live or potential WiFi session to a satellite. Slots are declarative: this
// holds the desired descriptor per slot plus the applied state the satellite last
// confirmed. Topology rides REST only, never UDP.
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

    // Rounded to the 0.1 ms display precision. 0 / 0 with no live session or an
    // unseeded window, and zeroed on teardown so a dead row shows no figure.
    double latencyOneWayMs() const { return latencyOneWayMs_; }
    int latencySamples() const { return latencySamples_; }

    // What the heartbeat-ack epoch is compared against. -1 until the first PUT.
    int lastAppliedEpoch() const { return lastAppliedEpoch_; }
    void adoptEpoch(int epoch) { lastAppliedEpoch_ = epoch; }
    // The server computes this grant only at session PUT, never per controller.
    bool mouseControlGranted() const { return mouseControlGranted_; }

    void updateServer(const models::DiscoveredServer& s);
    void markConnecting();
    // Adopts the UDP tuple and applied state from a session PUT response.
    // `onRekey` fires once per approach to the send-counter threshold, not once
    // per tick. All four callbacks arrive on the main thread's alive-poll.
    void markConnected(const std::shared_ptr<SatelliteClient>& client, const QString& connectionId,
                       int epoch, bool mouseControlGranted, std::function<void()> onDead,
                       std::function<void(std::uint8_t reason)> onClose,
                       std::function<void()> onReconcile, std::function<void()> onRekey);
    void markDisconnected();
    // markDisconnected but landing in Stale, for the terminal-401, unpaired and
    // silent-retry paths.
    void markStale();

    // `registered` means the satellite confirmed the virtual pad exists. Every
    // stream is gated on it.
    struct SlotBinding {
        int controllerIndex = 0;
        int controllerType = proto::kControllerTypeXbox;
        std::uint8_t touchpadMode = proto::kTouchpadModeOff;
        bool hasLightbar = false;
        bool hasMotion = false;
        // Path-resolved at bind: the SDL probe for a Standard slot, false for a
        // USB-direct claim (no output write path), so the satellite never
        // offers a rumble channel that cannot fire.
        bool hasRumble = false;
        bool registered = false;
    };

    // The type and touchpad mode travel with the attach, so there is no
    // default-then-correct phase. While live the manager converges it via a
    // controller PUT; while idle it rides the next session PUT.
    void attachSlot(const QString& slotId, int controllerType, bool hasLightbar, bool hasMotion,
                    bool hasRumble, std::uint8_t touchpadMode = proto::kTouchpadModeOff);
    void detachSlot();
    void detachSlot(const QString& slotId);

    std::optional<models::ControllerDescriptor> descriptorFor(const QString& slotId) const;
    // The controllers[] for a session PUT.
    QList<models::ControllerDescriptor> desiredDescriptors() const;
    QString slotIdForIndex(int ctrlIdx) const;
    // Drives hostFeatures on the session PUT.
    bool wantsMouseControl() const;

    void applyResults(const QList<models::ControllerApplyDto>& results);
    // True when the caller may adopt the epoch instead of re-PUTting.
    bool matchesAppliedView(const models::SessionViewDto& view) const;
    // Registered controller indices, for comparison against the ack's bitmap.
    std::uint16_t registeredBitmap() const;

    // Hot path, called on the SDL input thread. Silently drops unless the bound
    // slot is registered, since the server would discard it anyway.
    void sendReport(std::uint16_t buttons, std::uint8_t lt, std::uint8_t rt, std::int16_t lx,
                    std::int16_t ly, std::int16_t rx, std::int16_t ry);
    void sendMotion(std::int16_t gyroX, std::int16_t gyroY, std::int16_t gyroZ, std::int16_t accelX,
                    std::int16_t accelY, std::int16_t accelZ, std::uint32_t timestampDeltaUs);
    void sendBattery(std::uint8_t level, std::uint8_t status);
    void sendTouchpad(bool finger0Active, std::uint8_t finger0Id, std::int16_t finger0X,
                      std::int16_t finger0Y, bool finger1Active, std::uint8_t finger1Id,
                      std::int16_t finger1X, std::int16_t finger1Y, bool buttonPressed,
                      std::uint32_t eventTimeMs);

    // Held here rather than on the per-session SatelliteClient so they survive a
    // reconnect; markConnected re-installs them.
    using RumbleHandler = std::function<void(const SatelliteClient::RumbleMessage&)>;
    void setRumbleHandler(RumbleHandler handler);
    using LightbarHandler = std::function<void(const SatelliteClient::LightbarMessage&)>;
    void setLightbarHandler(LightbarHandler handler);

  signals:
    void changed();
    // Kept separate from `changed` because that fans out into the hub and
    // AppModel rebuild cascade, which a 1 Hz cosmetic tick must not thrash.
    void telemetryChanged();
    void errorOccurred(const QString& message);
    // The manager converges it via PUT /api/connections/{id}/controllers/{idx}.
    void slotChanged(const QString& slotId);
    // The manager removes it via DELETE the same path.
    void slotRemoved(int ctrlIdx);

  private:
    // Test-only seam to drive the alive tick without the 1 s timer.
    friend class WifiConnectionTestAccess;

    QString id_;
    models::DiscoveredServer server_;
    SessionState state_ = SessionState::Idle;
    std::optional<QString> connectionId_;
    std::optional<QString> boundSlotId_;
    int lastAppliedEpoch_ = -1;
    bool mouseControlGranted_ = false;
    // Main-thread only.
    double latencyOneWayMs_ = 0.0;
    int latencySamples_ = 0;

    // std::map, not QHash, so descriptor iteration order stays deterministic and
    // PUT bodies are reproducible.
    std::map<QString, SlotBinding> slots_;

    ClientRef clientRef_;
    QTimer* aliveTimer_ = nullptr;
    // Driven from the main-thread alive-poll, never a receive-thread callback, so
    // the session FSM and REST never run off the UDP receive thread.
    std::function<void()> onDead_;
    std::function<void(std::uint8_t reason)> onClose_;
    std::function<void()> onReconcile_;
    std::function<void()> onRekey_;
    // Re-armed only once the counter drops back under the threshold, so a slow or
    // failed re-PUT is not re-requested every tick.
    bool rekeyRequested_ = false;

    RumbleHandler rumbleHandler_;
    LightbarHandler lightbarHandler_;

    models::ControllerDescriptor descriptorOf(const SlotBinding& b) const;
    int lowestFreeIndex() const;
    void onAliveTick();
    void teardownClient();
};

} // namespace dish::net
