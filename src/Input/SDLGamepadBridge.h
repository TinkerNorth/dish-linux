// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#pragma once

#include "GamepadInputProcessor.h"
#include "JoystickMapping.h"
#include "OutputCommandQueue.h"

#include <QObject>
#include <QString>

#include <array>
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

// SDL's own declarations of the handle and event types named below.
// SDL_events.h is the smallest SDL header that declares both event structs; it
// brings the game-controller and joystick headers with it. Every consumer of
// this header already compiles against SDL.
#include <SDL2/SDL_events.h>

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

    // The DualSense surfaces SDL has no typed call for. Each is built into the
    // family's own OUT report by core/input/UsbOutputReports.h on the SDL
    // thread (the per-pad FeedbackState shadow lives there) and handed to
    // SDL_GameControllerSendEffect, which only SDL's HIDAPI drivers implement:
    // every other backend refuses it, and AppModel never routes here for one
    // of those (SlotFeedbackInputs::standardEffects). Queued like applyRumble.
    void
    applyTriggerEffects(const QString& deviceId,
                        const std::array<std::uint8_t, usbout::kTriggerEffectBlockBytes>& left,
                        const std::array<std::uint8_t, usbout::kTriggerEffectBlockBytes>& right);
    void applyPlayerLeds(const QString& deviceId, std::uint8_t ledMask);
    void applyMicLed(const QString& deviceId, std::uint8_t state);

  signals:
    void devicesChanged();

    // An audio ENDPOINT appeared or vanished (SDL_AUDIODEVICEADDED/REMOVED).
    // The bridge owns the one SDL event pump, so the events surface here, but
    // SDL_INIT_AUDIO's lifecycle belongs to SdlAudioGateway — this signal only
    // tells the pad-to-endpoint route resolver to re-run. Queued to the GUI
    // thread; no payload, since the resolver re-enumerates wholesale anyway.
    void audioDevicesChanged();

    // A raw joystick input observed while capture is enabled. `deviceId` is the
    // "sdl:<iid>" id; `kind` is 0=axis / 1=button / 2=hat; `index` is the raw
    // source index; `value` is the axis int16 / 1 for a button press / the
    // SDL_HAT_* bitmask for a hat. The GUI thread (AppModel → AppViewModel) maps
    // the deviceId to a slot and routes it to the output being assigned. Emitted
    // via QueuedConnection so it crosses from the SDL thread to the GUI thread.
    void rawJoystickInput(QString deviceId, int kind, int index, int value);

  private:
    void runLoop();

    // What one SDL event means, what a raw-joystick event means in particular, and what the loop
    // closes on its way out. All on the SDL thread.
    void dispatchSdlEvent(const SDL_Event& ev);
    void onRawJoystickInput(int iid, CaptureKind kind, int index, int value, bool deliberate);
    void closeAllDevices();

    // Long enough that an idle pump does not spin, short enough that a stop is observed promptly.
    static constexpr int kSdlWaitMs = 100;

    void applySdlHints();
    bool initSdl();
    void onControllerAdded(const SDL_Event& ev);
    struct ControllerCaps;
    static ControllerCaps probeControllerCaps(SDL_GameController* gc);
    void registerController(int iid, SDL_GameController* gc, const QString& deviceId,
                            const QString& deviceName, const ControllerCaps& caps);
    static void logControllerCaps(SDL_GameController* gc, SDL_Joystick* js, const QString& deviceId,
                                  const QString& deviceName, const ControllerCaps& caps);
    void onControllerRemoved(const SDL_Event& ev);
    void onJoystickAdded(const SDL_Event& ev);
    void onJoystickRemoved(const SDL_Event& ev);
    // Drain the pending-command queue and execute each SDL output call
    // (rumble / SetLED / SendEffect) on the SDL thread. Called once per
    // runLoop iteration.
    void drainOutputCommands();
    // Build one effect command's DualSense report and hand its body to
    // SDL_GameControllerSendEffect. SDL thread only.
    void sendEffect(SDL_GameController* gc, int iid, const OutputCommand& cmd);
    void rebuildState(int iid);
    // RAW-joystick twin of rebuildState: reads an open SDL_Joystick's current
    // raw axis/button/hat state, runs the SDL-free JoystickMapping default
    // layout, and publishes the resulting report — the SAME processor path the
    // game-controller rebuildState uses. Only for pads SDL does NOT recognise
    // as game controllers (see openJoysticks_).
    void rebuildJoystickState(int iid);

    // Fixed caps, so the per-event read allocates nothing. A pad with more inputs than a cap is
    // truncated, which loses nothing because the layouts reference only low indices.
    static constexpr int kMaxJoystickAxes = 32;
    static constexpr int kMaxJoystickButtons = 64;
    static constexpr int kMaxJoystickHats = 8;

    struct JoystickHandle;
    JoystickHandle joystickHandleFor(int iid);
    JoystickRemap remapFor(int vendorId, int productId);
    static JoystickSnapshot readJoystick(SDL_Joystick* js, std::int16_t (&axes)[kMaxJoystickAxes],
                                         bool (&buttons)[kMaxJoystickButtons],
                                         std::uint8_t (&hats)[kMaxJoystickHats]);
    void handleSensorEvent(const SDL_ControllerSensorEvent& ev);
    void handleTouchpadEvent(const SDL_ControllerTouchpadEvent& ev);
    void pollBatteries();

    using TimePoint = std::chrono::steady_clock::time_point;

    // One device whose charge is due to be read, with its joystick handle already resolved so the
    // publish needs no lock of its own.
    struct PollEntry {
        int iid = 0;
        std::string deviceId;
        SDL_Joystick* js = nullptr;
    };
    std::vector<PollEntry> batteriesDue(TimePoint now);
    void considerForPoll(int iid, SDL_Joystick* js, TimePoint now, std::vector<PollEntry>& due);
    bool publishBattery(const PollEntry& e);
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

    // Per-pad shadow for the effect builders (the DS5 lamp re-assert), the
    // twin of UsbGamepadManager's per-claim one. SDL-thread only: written at
    // drain, dropped at detach, never read under mtx_.
    std::unordered_map<int, usbout::FeedbackState> effectState_;

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

    // What one sensor event leaves behind, when it leaves anything: the slot's id and the accel
    // triple the gyro sample is paired with.
    struct MotionUpdate {
        std::string deviceId;
        AccelCache accel{};
    };
    std::optional<MotionUpdate> applySensorEvent(const SDL_ControllerSensorEvent& ev);

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

    // What one touchpad event leaves behind: the slot's id, the tracked finger state, and the
    // controller handle the click is read from, outside the lock.
    struct TouchUpdate {
        std::string deviceId;
        SDL_GameController* controller = nullptr;
        TouchState state;
    };
    std::optional<TouchUpdate> applyTouchpadEvent(const SDL_ControllerTouchpadEvent& ev);
    static void applyTouchFinger(TouchState& ts, const SDL_ControllerTouchpadEvent& ev);
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
