// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#pragma once

#include "GamepadInputProcessor.h"
#include "JoystickMapping.h"
#include "OutputCommandQueue.h"

#include <QObject>
#include <QString>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

// Mirrors SDL2's typedefs so <SDL.h> stays out of this header. The leading
// underscores are SDL's struct tags, not our choice.
extern "C" {
// NOLINTNEXTLINE(bugprone-reserved-identifier)
struct _SDL_GameController;
using SDL_GameController = struct _SDL_GameController;
// NOLINTNEXTLINE(bugprone-reserved-identifier)
struct _SDL_Joystick;
using SDL_Joystick = struct _SDL_Joystick;
struct SDL_ControllerSensorEvent;
struct SDL_ControllerTouchpadEvent;
}

namespace dish::input {

// Pumps SDL_GameController events on a dedicated thread and forwards each
// state change to GamepadInputProcessor::publish from that same thread — the
// report flushes straight out of the input callback, never via a Qt event hop.
class SDLGamepadBridge : public QObject {
    Q_OBJECT
  public:
    explicit SDLGamepadBridge(GamepadInputProcessor* processor, QObject* parent = nullptr);
    ~SDLGamepadBridge() override;

    void start();
    void stop();

    // A snapshot of one attached device. Every field is classified once at
    // attach except the battery pair, which pollBatteries() refreshes.
    struct Device {
        QString id;
        QString name;
        bool motionCapable = false;
        bool hasLightbar = false;
        // 0..100 or 0xFF (unknown); `batteryStatus` is a kBatteryStatus*
        // constant. 0xFF / 0 until the first poll completes.
        std::uint8_t batteryLevel = 0xFF;
        std::uint8_t batteryStatus = 0;
        // 0 when SDL could not report it. Lets AppModel pair this device with a
        // USB-direct raw-HID twin of the same model — see setSuppressedDeviceIds.
        int vendorId = 0;
        int productId = 0;
        // Only raw joysticks decode through the mapJoystick / JoystickRemap
        // path, so only they are remappable; a game controller uses SDL's own
        // mapping and ignores any remap.
        bool isRawJoystick = false;
        // A pad with no touch source always declares touchpadMode "off".
        bool hasTouchpad = false;
        // Whether SDL can actually drive this pad's motors (the probe, not the
        // model DB) — what CAP_RUMBLE advertises for a Standard-path slot.
        bool hasRumble = false;
        // Classified at attach from SDL's device path. Gates the USB-path stamp
        // in AppModel::rebuild — a wireless pad has no USB path to switch.
        // False when SDL reports no path, which leaves
        // the wired presentation in place and so fails safe.
        bool bluetooth = false;
    };
    QList<Device> devices() const;

    // ── USB-direct twin-dedup seam ───────────────────────────────────────────
    // A pad visible to BOTH SDL and the raw-HID USB-direct gateway must
    // stream via exactly one path. AppModel installs the SDL ids that are twins
    // of an active USB-direct synthetic; the input thread then skips
    // publish()/publishMotion()/publishTouchpad() for them. On claim-failure or
    // detach the set recomputes without that id and SDL resumes.
    //
    // Written from the Qt main thread, read on the input thread under
    // suppressedMtx_. Empty by default, so nothing is suppressed until asked.
    void setSuppressedDeviceIds(const std::unordered_set<std::string>& ids);

    // ── Per-(vid,pid) raw-joystick REMAP seam ────────────────────────────────
    // rebuildJoystickState maps under the remap stored for the device's
    // (vid,pid), or the default JoystickRemap when none is set. Written from
    // the Qt main thread, read on the input thread under remapMtx_; the hot
    // path copies the small remap under the lock and maps OUTSIDE it.
    void setJoystickRemap(int vendorId, int productId, const input::JoystickRemap& remap);
    void clearJoystickRemap(int vendorId, int productId);

    // ── Input-capture seam (the "press a button to assign it" mode) ──────────
    // When ON, the JOY event cases additionally emit rawJoystickInput so the
    // remap page can tell WHICH raw input the user pressed. Streaming continues
    // during capture, and the flag costs a relaxed atomic load when off — no
    // lock is added to the hot path.
    void setJoystickCaptureEnabled(bool enabled);

    // Vibration only; never touches the LED. Magnitudes are on the XUSB 16-bit
    // scale so they pass through SDL2 verbatim, and `durationMs == 0` is
    // forwarded as-is because SDL already reads it as "stop".
    //
    // Callable from any thread (in practice a SatelliteClient receive thread):
    // the request is queued and runLoop() makes the SDL call on the SDL thread,
    // resolving the SDL_GameController* fresh there. A controller closed in the
    // meantime is skipped, so there is no use-after-close race.
    void applyRumble(const QString& deviceId, std::uint16_t strongMagnitude,
                     std::uint16_t weakMagnitude, std::uint16_t durationMs);

    // The sole lightbar entry point; a no-op for pads without an LED. Queued
    // onto the SDL thread exactly like applyRumble.
    void applyLightbar(const QString& deviceId, std::uint8_t r, std::uint8_t g, std::uint8_t b);

  signals:
    void devicesChanged();

    // A raw joystick input observed while capture is enabled. `deviceId` is the
    // "sdl:<iid>" id; `kind` is 0=axis / 1=button / 2=hat; `index` is the raw
    // source index; `value` is the axis int16 / 1 for a button press / the
    // SDL_HAT_* bitmask for a hat. The GUI thread (AppModel → AppViewModel) maps
    // the deviceId to a slot and routes it to the output being assigned. Emitted
    // via QueuedConnection so it crosses from the SDL thread to the GUI thread.
    void rawJoystickInput(QString deviceId, int kind, int index, int value);

  private:
    void runLoop();
    // Drain the pending-command queue and execute each SDL output call
    // (rumble / SetLED) on the SDL thread. Called once per runLoop iteration.
    void drainOutputCommands();
    void rebuildState(int iid);
    // RAW-joystick twin of rebuildState: reads an open SDL_Joystick's current
    // raw axis/button/hat state, runs the SDL-free JoystickMapping default
    // layout, and publishes the resulting report — the SAME processor path the
    // game-controller rebuildState uses. Only for pads SDL does NOT recognise
    // as game controllers (see openJoysticks_).
    void rebuildJoystickState(int iid);
    void handleSensorEvent(const SDL_ControllerSensorEvent& ev);
    void handleTouchpadEvent(const SDL_ControllerTouchpadEvent& ev);
    void pollBatteries();
    // True iff `deviceId` is currently twin-suppressed (USB-direct owns the pad).
    // Cheap: a short-held read of suppressedIds_ under suppressedMtx_.
    bool isSuppressed(const std::string& deviceId) const;

    GamepadInputProcessor* processor_;
    std::thread thread_;
    std::atomic<bool> running_{false};

    // Guarded by mtx_; manipulated only from the input thread except for
    // devices() which reads under lock.
    mutable std::mutex mtx_;
    std::unordered_map<int, SDL_GameController*> openControllers_;
    std::unordered_map<int, QString> deviceIds_;
    std::unordered_map<int, QString> deviceNames_;

    // RAW-JOYSTICK fallback: SDL joysticks that are NOT game controllers (no
    // entry in SDL's mapping DB — e.g. a generic vid 0x0079 pad). These are
    // opened with SDL_JoystickOpen and stream through rebuildJoystickState +
    // the SDL-free JoystickMapping default layout, NOT the game-controller
    // path. Tracked in their OWN map so they never collide with
    // openControllers_; their instance ids are disjoint from it by the
    // SDL_IsGameController guard in runLoop (a game controller is opened on the
    // controller path, a non-controller joystick here — never both). They share
    // deviceIds_ / deviceNames_ / usbIdentity_ / lastBattery_ with the
    // controller path so devices() emits one unified list and no extra plumbing
    // is needed in AppModel::rebuild. They are intentionally absent from
    // motionCapable_ / lightbarCapable_ (a raw joystick exposes neither IMU nor
    // LED through SDL's joystick API). Same lifecycle / locking as
    // openControllers_.
    std::unordered_map<int, SDL_Joystick*> openJoysticks_;

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
    // Instance ids whose pad exposes a readable touchpad (SDL
    // GetNumTouchpads > 0). Same lifecycle / locking as motionCapable_.
    std::unordered_set<int> touchpadCapable_;
    // Instance ids whose motors SDL can drive (SDL HasRumble — the probe is
    // authoritative for the Standard path). Same lifecycle / locking as
    // motionCapable_.
    std::unordered_set<int> rumbleCapable_;
    // Instance ids attached over Bluetooth (classified once at attach from the
    // SDL device path). Surfaced through devices() as Device::bluetooth. Same
    // lifecycle / locking as motionCapable_.
    std::unordered_set<int> bluetoothIids_;

    // Per-device USB identity (vid, pid) classified once at attach. Surfaced via
    // devices() for the twin-dedup pairing. Same lifecycle / locking as
    // motionCapable_; a device absent reads (0, 0).
    struct UsbIdentity {
        int vendorId = 0;
        int productId = 0;
    };
    std::unordered_map<int, UsbIdentity> usbIdentity_;

    // The set of device ids whose SDL input is twin-suppressed because a
    // USB-direct claim of the same model is streaming (see setSuppressedDeviceIds
    // / UsbTwinDedup). Guarded by its OWN mutex (not mtx_) so installing a fresh
    // set on the main thread never contends with the device-map critical section
    // on the input thread, and the per-report read is a tiny independent lock.
    mutable std::mutex suppressedMtx_;
    std::unordered_set<std::string> suppressedIds_;

    // Per-(vid,pid) raw-joystick remap overrides (see setJoystickRemap). Keyed by
    // a packed (vendorId, productId) pair. Guarded by its OWN mutex so a main-
    // thread push never contends with the device-map critical section; the hot
    // path copies the matched remap under this lock and maps outside it. A device
    // absent from the map decodes under the default JoystickRemap.
    mutable std::mutex remapMtx_;
    std::map<std::pair<int, int>, input::JoystickRemap> joystickRemaps_;

    // Capture mode flag (see setJoystickCaptureEnabled). A relaxed atomic so the
    // JOY event cases can gate the emit with no lock when capture is off (the
    // overwhelmingly common case).
    std::atomic<bool> captureEnabled_{false};

    // Emit a raw-input capture for `iid` if capture is enabled. Resolves the iid
    // to its deviceId under mtx_ and emits rawJoystickInput on the GUI thread.
    // Called from the JOY event cases on the SDL thread; a cheap no-op when off.
    void maybeEmitCapture(int iid, int kind, int index, int value);

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
    // `id` is the protocol's monotonic per-finger tracking id: it is bumped
    // once on every fresh contact (the SDL_CONTROLLERTOUCHPADDOWN false→true
    // edge) so the receiver can correlate a finger across frames even when the
    // other finger lifts. It wraps freely (uint8). `id` is per finger-slot;
    // the two slots advance independently.
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
