// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#pragma once

#include "GamepadInputProcessor.h"
#include "OutputCommandQueue.h"

#include <QObject>
#include <QString>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

// Mirror SDL2's own typedef so we can keep <SDL.h> out of this header.
// The leading underscore is dictated by SDL's struct tag, not our choice.
extern "C" {
// NOLINTNEXTLINE(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp)
struct _SDL_GameController;
using SDL_GameController = struct _SDL_GameController;
struct SDL_ControllerSensorEvent;
struct SDL_ControllerTouchpadEvent;
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

    // List of currently-attached devices in (deviceId, displayName) form,
    // plus whether the device exposes a motion sensor (gyro/accelerometer).
    // `motionCapable` is true only for pads that have an IMU and had at least
    // one of SDL_SENSOR_GYRO / SDL_SENSOR_ACCEL successfully enabled — i.e.
    // DualSense / DS4 / Switch-class pads, never Xbox 360 / Xbox One pads.
    //
    // `hasLightbar` is true iff SDL_GameControllerHasLED reported an
    // addressable RGB LED for the device (DualSense / DualShock 4). It drives
    // the SlotCard lightbar chip and the CAP_LIGHTBAR bit in MSG_CONTROLLER_ADD.
    //
    // `batteryLevel` / `batteryStatus` carry the most recent battery sample
    // for the device — the same (level, status) pair pollBatteries() forwards
    // onto the wire (the controller's own charge for a wireless pad, the host
    // machine's for a wired/unknown one). They drive the SlotCard battery
    // chip. `batteryLevel` is 0..100 or 0xFF (unknown); `batteryStatus` is a
    // kBatteryStatus* constant. 0xFF / 0 until the first poll completes.
    struct Device {
        QString id;
        QString name;
        bool motionCapable = false;
        bool hasLightbar = false;
        std::uint8_t batteryLevel = 0xFF;
        std::uint8_t batteryStatus = 0;
    };
    QList<Device> devices() const;

    // Drive the physical controller's rumble motors — vibration ONLY. As of
    // Task 1.4 the lightbar is fully decoupled from rumble: this never touches
    // the LED. `strongMagnitude` and `weakMagnitude` are 16-bit magnitudes
    // matching XInput's scale so they can flow through the SDL2 API verbatim.
    // `durationMs == 0` is a "stop" signal — SDL itself treats 0 as "do not
    // run", so we forward as-is.
    //
    // Thread-safety: callable from any thread. The SDL call is NOT made here:
    // the request is pushed onto an internal mutex-guarded command queue and
    // executed by runLoop() on the SDL thread, where the SDL_GameController*
    // is resolved fresh — a controller closed in the meantime is simply
    // skipped, so there is no use-after-close race. Intended to be invoked
    // from the SatelliteClient receive thread.
    void applyRumble(const QString& deviceId, std::uint16_t strongMagnitude,
                     std::uint16_t weakMagnitude, std::uint16_t durationMs);

    // Drive the physical controller's LED to the given colour. The sole
    // lightbar entry point: the dedicated MSG_LIGHTBAR stream routes here.
    // No-op for pads without an LED.
    //
    // Thread-safety: same as applyRumble — the SDL_GameControllerSetLED call
    // is marshalled onto the SDL thread via the command queue rather than
    // made on the caller's (receive) thread.
    void applyLightbar(const QString& deviceId, std::uint8_t r, std::uint8_t g, std::uint8_t b);

  signals:
    void devicesChanged();

  private:
    void runLoop();
    // Drain the pending-command queue and execute each SDL output call
    // (rumble / SetLED) on the SDL thread. Called once per runLoop iteration.
    void drainOutputCommands();
    void rebuildState(int iid);
    void handleSensorEvent(const SDL_ControllerSensorEvent& ev);
    void handleTouchpadEvent(const SDL_ControllerTouchpadEvent& ev);
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

    // Devices for which SDL_GameControllerHasLED returned true at attach
    // (DualSense / DualShock 4). Surfaced through devices() as Device::
    // hasLightbar so the UI can show a lightbar chip and WifiConnection can
    // advertise CAP_LIGHTBAR. Same lifecycle / locking as motionCapable_.
    std::unordered_set<int> lightbarCapable_;

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

    // Per-device most recent battery sample (level, status). Updated by
    // pollBatteries() and surfaced through devices() so the SlotCard can
    // render a battery chip. Guarded by mtx_ like the other device maps.
    struct BatterySnapshot {
        std::uint8_t level = 0xFF;
        std::uint8_t status = 0;
    };
    std::unordered_map<int, BatterySnapshot> lastBattery_;

    // Per-device touchpad finger state. SDL delivers per-finger down/move/up
    // events; we accumulate them here and emit the full two-finger snapshot
    // on every change (MSG_TOUCHPAD carries both fingers at once).
    // Input-thread-only. `x`/`y` are already scaled to the wire int16.
    //
    // `id` is the protocol's monotonic per-finger tracking id: the receiver
    // tells a fresh touch apart from a continuation by it. It is bumped on
    // each contact edge (a SDL_CONTROLLERTOUCHPADDOWN that flips this slot
    // from inactive to active), wrapping freely as a uint8_t — see
    // handleTouchpadEvent. It rides MSG_TOUCHPAD's fingerN_trackingId byte.
    struct TouchFinger {
        bool active = false;
        std::uint8_t id = 0;
        std::int16_t x = 0;
        std::int16_t y = 0;
    };
    struct TouchState {
        TouchFinger fingers[2];
    };
    std::unordered_map<int, TouchState> touchState_;

    // Cross-thread output-command marshalling (Task 1.4 threading fix).
    //
    // SDL_GameControllerRumble / SDL_GameControllerSetLED must run on the SDL
    // thread (the one in runLoop) because the SDL thread is also the only
    // thread allowed to SDL_GameControllerClose a controller. applyRumble /
    // applyLightbar are called from the SatelliteClient receive thread; if
    // they called the SDL function directly they could race a close and use a
    // freed SDL_GameController*. Instead they push onto this queue and runLoop
    // drains it, resolving the device id → controller on the SDL thread (a
    // closed controller is just skipped). OutputCommandQueue carries its own
    // lock — independent of mtx_, so a flood of rumble packets on the receive
    // thread never contends with the device map.
    OutputCommandQueue outputQueue_;
};

} // namespace dish::input
