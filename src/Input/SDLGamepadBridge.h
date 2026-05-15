// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#pragma once

#include "GamepadInputProcessor.h"

#include <QObject>
#include <QString>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <unordered_map>
#include <unordered_set>

// Mirror SDL2's own typedef so we can keep <SDL.h> out of this header.
// The leading underscore is dictated by SDL's struct tag, not our choice.
extern "C" {
// NOLINTNEXTLINE(bugprone-reserved-identifier)
struct _SDL_GameController;
using SDL_GameController = struct _SDL_GameController;
struct SDL_ControllerSensorEvent;
}

namespace dish::input {

// Pumps SDL_GameController events on a dedicated thread and forwards each
// state change to GamepadInputProcessor::publish from that same thread —
// matching the dish-mac/Android pattern where the report flushes directly out
// of the input callback for minimum latency.
class SDLGamepadBridge : public QObject {
    Q_OBJECT
  public:
    explicit SDLGamepadBridge(GamepadInputProcessor* processor, QObject* parent = nullptr);
    ~SDLGamepadBridge() override;

    void start();
    void stop();

    // List of currently-attached devices in (deviceId, displayName) form.
    struct Device {
        QString id;
        QString name;
    };
    QList<Device> devices() const;

    // Drive the physical controller's rumble motors. `strongMagnitude` and
    // `weakMagnitude` are 16-bit magnitudes matching XInput's scale so they
    // can flow through the SDL2 API verbatim. `durationMs == 0` is a "stop"
    // signal — SDL itself treats 0 as "do not run", so we forward as-is.
    //
    // If the controller exposes a lightbar (DualShock 4 / DualSense) and the
    // satellite published one, we also call SDL_GameControllerSetLED. Failures
    // are silent — many pads don't support either operation and SDL just
    // returns -1 in that case.
    //
    // Thread-safety: callable from any thread; takes the same internal mutex
    // that guards the device map. Intended to be invoked from the
    // SatelliteClient receive thread.
    void applyRumble(const QString& deviceId, std::uint16_t strongMagnitude,
                     std::uint16_t weakMagnitude, std::uint16_t durationMs, bool hasLightbar,
                     std::uint8_t lightbarR, std::uint8_t lightbarG, std::uint8_t lightbarB);

  signals:
    void devicesChanged();

  private:
    void runLoop();
    void rebuildState(int iid);
    void handleSensorEvent(const SDL_ControllerSensorEvent& ev);
    void pollBatteries();

    GamepadInputProcessor* processor_;
    std::thread thread_;
    std::atomic<bool> running_{false};

    // Guarded by mtx_; manipulated only from the input thread except for
    // devices() which reads under lock.
    mutable std::mutex mtx_;
    std::unordered_map<int, SDL_GameController*> openControllers_;
    std::unordered_map<int, QString> deviceIds_;
    std::unordered_map<int, QString> deviceNames_;

    // Devices that successfully had at least one of SDL_SENSOR_GYRO /
    // SDL_SENSOR_ACCEL enabled. Used to skip the sensor-event dispatch
    // overhead for Xbox 360 / Xbox One pads. Manipulated only on the
    // input thread, but the read in handleSensorEvent goes through mtx_.
    std::unordered_set<int> motionCapable_;

    // Latest accelerometer reading per device (m/s²). Updated when an accel
    // SDL_CONTROLLERSENSORUPDATE arrives; merged with the next gyro update
    // into a single MotionSample. Input-thread-only.
    struct AccelCache {
        float ax = 0.0f;
        float ay = 0.0f;
        float az = 0.0f;
    };
    std::unordered_map<int, AccelCache> lastAccel_;

    // Per-device last battery poll wall-clock. The runLoop polls battery on
    // every iteration but the per-device gate collapses it to 30 s.
    std::unordered_map<int, std::chrono::steady_clock::time_point> lastBatteryPoll_;
};

} // namespace dish::input
