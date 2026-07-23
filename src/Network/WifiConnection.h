// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#pragma once

#include "Models/Models.h"
#include "SatelliteClient.h"

#include <QObject>
#include <QString>
#include <QTimer>

#include <cstdint>
#include <functional>
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
// - Faltering  — Live, but the consecutive missed-ack count has reached the
//                "not responding" threshold (2) without hitting death (5).
//                The alive tick flips Live ⇄ Faltering from
//                SatelliteClient::missedAcks(). UI chip: "Unsteady".
enum class SessionState : std::uint8_t { Idle, Linking, Live, Faltering };

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

// A single live or potential WiFi session to one Satellite server.
//
// Protocol-1: topology is REST-only. This class keeps the binding state
// (which slot, which type, which capability bits) and drives the
// per-controller converge through hooks the manager installs at
// markConnected() — it never talks HTTP itself. The old UDP
// CONTROLLER_ADD/ACK polling is gone.
class WifiConnection : public QObject {
    Q_OBJECT
  public:
    static QString idFor(const models::DiscoveredServer& s) { return s.id(); }

    // Control-plane callbacks installed by WifiConnectionManager at
    // markConnected(). All fire on the Qt main thread.
    struct SessionHooks {
        // Heartbeat death (miss threshold hit) — manager schedules the
        // backoff retry and tears the session down.
        std::function<void()> onDead;
        // A SESSION_CLOSE (0x000F) reason latched on the client — manager
        // maps it through reducer::closeActionForReason.
        std::function<void(std::uint8_t reason)> onClose;
        // PUT /api/connections/{id}/controllers/{idx} with the descriptor;
        // the manager's wrapper centralises the terminal-401 check before
        // forwarding the response here.
        std::function<void(const models::ControllerDescriptor&,
                           std::function<void(const models::ControllerPutResponse&)>)>
            putSlot;
        // DELETE /api/connections/{id}/controllers/{idx} (fire-and-forget).
        std::function<void(int ctrlIdx)> deleteSlot;
        // Enriched-ack epoch/bitmap drifted from what we applied — manager
        // runs the GET-then-converge reconcile (single-flight guarded here).
        std::function<void()> reconcile;
        // Send counter crossed the proactive re-PUT threshold — manager
        // re-PUTs for fresh token/salt/key before the counter can exhaust
        // (single-fire per approach, guarded here).
        std::function<void()> rekey;
    };

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
    void markConnected(const std::shared_ptr<SatelliteClient>& client, const QString& connectionId,
                       int epoch, SessionHooks hooks);
    void markDisconnected();

    // Bind this connection to a controller slot. `hasLightbar` gates
    // CAP_LIGHTBAR (0x0008) and `hasMotion` CAP_MOTION (0x0004) in the
    // descriptor caps word; both are stored so a later registration (on
    // reconnect) advertises the same capabilities. The emulated type itself
    // comes from the satellite catalog (setCatalog), not the physical slot.
    void attachSlot(const QString& slotId, bool hasLightbar, bool hasMotion);
    void detachSlot();

    // The satellite's controller-type catalog, fetched once by the manager
    // before the session PUT. desiredDescriptor() defaults the sent type to its
    // first entry (physical-pad matching deferred) and drives touchpadMode off
    // it; an empty catalog (unreachable/older satellite) falls back to type 0.
    void setCatalog(const models::ServerCatalog& catalog);
    bool catalogFetched() const { return catalogFetched_; }

    bool isRegisteringController() const { return controllerRegistering_; }

    // The declarative descriptor for the bound slot (nullopt when unbound) —
    // rides the session PUT's controllers[] and the reconcile compare.
    std::optional<models::ControllerDescriptor> desiredDescriptor() const;

    // The session epoch we last applied (from the PUT / controller-PUT
    // response). Compared against the enriched ack's epoch by the alive tick.
    int lastAppliedEpoch() const { return lastAppliedEpoch_; }
    void setLastAppliedEpoch(int epoch) { lastAppliedEpoch_ = epoch; }

    // Single-flight guard for the manager's reconcile (set true when a GET is
    // launched, false when it lands). The alive tick skips re-triggering
    // while a reconcile is already in flight.
    bool reconcileInFlight() const { return reconcileInFlight_; }
    void setReconcileInFlight(bool inFlight) { reconcileInFlight_ = inFlight; }

    // Marks the slot converged after a session-level PUT already carried the
    // descriptor (openSession path) so attachSlot/markConnected don't issue a
    // duplicate per-slot PUT.
    void markSlotApplied();

    // One-way latency readout cached from the alive tick (median RTT / 2,
    // rounded to 0.1 ms). samples == 0 until the first ack lands. Refreshes
    // fire telemetryChanged(), NOT changed() — a 1 Hz cosmetic tick must not
    // trigger the wholesale UI rebuild (it would clobber list selection).
    double latencyOneWayMs() const { return latencyOneWayMs_; }
    int latencySamples() const { return latencySamples_; }

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
    // `eventTimeMs` is the SDL event timestamp (protocol-1 requires it).
    void sendTouchpad(bool finger0Active, std::uint8_t finger0Id, std::int16_t finger0X,
                      std::int16_t finger0Y, bool finger1Active, std::uint8_t finger1Id,
                      std::int16_t finger1X, std::int16_t finger1Y, bool buttonPressed,
                      std::uint32_t eventTimeMs);

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
    // 1 Hz latency/telemetry refresh — deliberately separate from changed()
    // (see latencyOneWayMs). Rows patch their label in place on this.
    void telemetryChanged();
    void errorOccurred(const QString& message);
    // Emitted when the slot converge for `slotId` was rejected or the server
    // never answered. Listened to by ConnectionHub to roll back the local
    // binding so the UI reflects reality.
    void registrationFailed(const QString& slotId);

  private:
    // Test-only seam so the rekey wiring is drivable without the 1 Hz timer.
    // Declared but never defined in production (SatelliteClient pattern).
    friend class WifiConnectionTestAccess;

    static constexpr int kDefaultCtrlIndex = 0;
    // Base capability word in the descriptor: analog triggers (0x0001) |
    // rumble (0x0002). CAP_MOTION / CAP_LIGHTBAR are per-controller and OR-ed
    // in from motionCapable_ / lightbarCapable_.
    static constexpr std::uint16_t kDefaultCaps =
        SatelliteClient::kCapAnalogTriggers | SatelliteClient::kCapRumble;

    void registerController();
    void onAliveTick();
    // The emulated controller type the catalog dictates: its first offered type
    // (interim default-to-first), or Xbox when no catalog has loaded.
    std::uint8_t selectedControllerType() const;

    QString id_;
    models::DiscoveredServer server_;
    SessionState state_ = SessionState::Idle;
    std::optional<QString> connectionId_;
    std::optional<QString> boundSlotId_;

    ClientRef clientRef_;
    QTimer* aliveTimer_ = nullptr;
    bool controllerRegistering_ = false;
    SessionHooks hooks_;
    bool controllerAdded_ = false;
    int lastAppliedEpoch_ = -1;
    bool reconcileInFlight_ = false;
    // Single-fire latch for hooks_.rekey: re-armed only once the re-key lands
    // (the fresh counter drops back under the threshold), so a slow/failed
    // re-PUT is not re-requested every tick.
    bool rekeyRequested_ = false;
    // Whether the bound slot's physical pad has an addressable RGB LED /
    // gyro+accel. Set by attachSlot; consumed by desiredDescriptor().
    bool lightbarCapable_ = false;
    bool motionCapable_ = false;

    // The satellite's offered controller-type catalog + whether it has been
    // fetched (once per connection lifetime — it survives reconnects so retries
    // against an unreachable satellite don't re-pay the GET). desiredDescriptor
    // sources the emulated type + touchpad mode from it.
    models::ServerCatalog catalog_;
    bool catalogFetched_ = false;

    double latencyOneWayMs_ = 0.0;
    int latencySamples_ = 0;

    // Set once during composition; re-applied to each fresh SatelliteClient
    // in markConnected() so we don't lose rumble across reconnects.
    RumbleHandler rumbleHandler_;
    LightbarHandler lightbarHandler_;
};

} // namespace dish::net
