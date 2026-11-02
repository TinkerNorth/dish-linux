// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#pragma once

#include "ControllerShadow.h"
#include "GamepadInputProcessor.h"
#include "JoystickMapping.h"
#include "core/input/DirtySet.h"
#include "core/input/OutputMailbox.h"
#include "core/input/UsbOutputReports.h"
#include "core/input/PumpPacing.h"
#include "core/thread/LatestValue.h"
#include "core/thread/TrackedMutex.h"

#include <QList>
#include <QObject>
#include <QString>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
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
//
// Every open pad's record belongs to the SDL thread, which takes no lock: the GUI reads the
// device list as a snapshot the SDL thread hands over on attach, detach and battery change, and
// hands the SDL thread its inputs (the twin-suppressed ids, the remaps) the same way back.
class SDLGamepadBridge : public QObject {
    Q_OBJECT
  public:
    explicit SDLGamepadBridge(GamepadInputProcessor* processor, QObject* parent = nullptr);
    ~SDLGamepadBridge() override;

    // The policy is read once here; the SDL thread is its only reader.
    void start(pump::PumpPolicy policy = {});
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
        // GUID plus the serial where the driver exposes one (a Sony pad's is its
        // Bluetooth address), so a standing binding follows the pad and not the
        // order pads were plugged.
        QString identity{};
    };
    // The latest list the SDL thread handed over, sorted by instance id. Any thread but the SDL
    // thread; readers take bridge.devices among themselves.
    QList<Device> devices() const;

    // ── USB-direct twin-dedup seam ───────────────────────────────────────────
    // A pad visible to BOTH SDL and the raw-HID USB-direct gateway must
    // stream via exactly one path. AppModel installs the SDL ids that are twins
    // of an active USB-direct synthetic; the input thread then skips
    // publish()/publishMotion()/publishTouchpad() for them. On claim-failure or
    // detach the set recomputes without that id and SDL resumes. Empty by
    // default, so nothing is suppressed until asked.
    void setSuppressedDeviceIds(const std::unordered_set<std::string>& ids);

    // ── Per-(vid,pid) raw-joystick REMAP seam ────────────────────────────────
    // rebuildJoystickState maps under the remap stored for the device's
    // (vid,pid), or the default JoystickRemap when none is set.
    void setJoystickRemap(int vendorId, int productId, const input::JoystickRemap& remap);
    void clearJoystickRemap(int vendorId, int productId);

    // ── Input-capture seam (the "press a button to assign it" mode) ──────────
    // When ON, the JOY event cases additionally emit rawJoystickInput so the
    // remap page can tell WHICH raw input the user pressed. Streaming continues
    // during capture, and the flag costs a relaxed atomic load when off.
    void setJoystickCaptureEnabled(bool enabled);

    // Feedback for a pad, by its processor device index. Callable from any thread (in practice a
    // SatelliteClient receive thread): the newest value of each kind waits in the device's mailbox
    // and the next pump cycle makes the SDL call on the SDL thread, so a burst collapses to its
    // last value and nothing queues. A device the bridge does not hold drops it there.
    //
    // Rumble is vibration only and never touches the LED. Magnitudes are on the XUSB 16-bit scale
    // so they pass through SDL2 verbatim, and `durationMs == 0` is forwarded as-is because SDL
    // already reads it as "stop". The lightbar is a no-op for pads without an LED.
    void postRumble(GamepadInputProcessor::DeviceIndex device, std::uint16_t strongMagnitude,
                    std::uint16_t weakMagnitude, std::uint16_t durationMs);
    void postLightbar(GamepadInputProcessor::DeviceIndex device, std::uint8_t r, std::uint8_t g,
                      std::uint8_t b);

    // The DualSense surfaces SDL has no typed call for. Each is built into the
    // family's own OUT report by core/input/UsbOutputReports.h on the SDL
    // thread (the per-pad FeedbackState shadow lives there) and handed to
    // SDL_GameControllerSendEffect, which only SDL's HIDAPI drivers implement:
    // every other backend refuses it, and the router never routes here for one
    // of those (SlotFeedbackInputs::standardEffects).
    void
    postTriggerEffects(GamepadInputProcessor::DeviceIndex device,
                       const std::array<std::uint8_t, usbout::kTriggerEffectBlockBytes>& left,
                       const std::array<std::uint8_t, usbout::kTriggerEffectBlockBytes>& right);
    void postPlayerLeds(GamepadInputProcessor::DeviceIndex device, std::uint8_t ledMask);
    void postMicLed(GamepadInputProcessor::DeviceIndex device, std::uint8_t state);

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
    using TimePoint = std::chrono::steady_clock::time_point;

    void runLoop();
    // One pump cycle: every queued SDL event, then the output queue, then the batteries when their
    // sweep is due.
    void pumpOnce(TimePoint now);

    // With no pad open the loop blocks in SDL_WaitEventTimeout as it always did: long enough that
    // an idle pump does not spin, short enough that a stop is observed promptly.
    static constexpr int kSdlWaitMs = 100;
    static constexpr int kPeepBatch = 32;
    static constexpr std::chrono::seconds kBatterySweepInterval{1};
    static constexpr std::size_t kMaxPads = 32;

    // What the GUI hands the SDL thread.
    struct Inputs {
        std::unordered_set<std::string> suppressed;
        std::map<std::pair<int, int>, JoystickRemap> remaps;
    };

    enum class PadKind : std::uint8_t { Free, Controller, Joystick };
    struct AccelCache {
        float ax = 0.0f;
        float ay = 0.0f;
        float az = 0.0f;
    };
    // `id` is the protocol's monotonic per-finger tracking id: it is bumped
    // once on every fresh contact (the SDL_CONTROLLERTOUCHPADDOWN false→true
    // edge) so the receiver can correlate a finger across frames even when the
    // other finger lifts. It wraps freely (uint8). `x`/`y` are already scaled to
    // the wire int16.
    struct TouchFinger {
        bool active = false;
        std::uint8_t id = 0;
        std::int16_t x = 0;
        std::int16_t y = 0;
    };
    struct TouchState {
        std::array<TouchFinger, 2> fingers{};
    };

    // One open pad, everything the SDL thread knows about it.
    struct Pad {
        int iid = -1;
        PadKind kind = PadKind::Free;
        SDL_GameController* gc = nullptr;
        SDL_Joystick* js = nullptr;
        std::string id;
        std::optional<GamepadInputProcessor::DeviceIndex> index;
        Device device;
        bool suppressed = false;
        JoystickRemap remap{};
        ControllerShadow shadow;
        std::optional<AccelCache> accel;
        TouchState touch;
        TimePoint lastBatteryPoll{};
        usbout::FeedbackState effectState{};
    };

    void dispatchSdlEvent(const SDL_Event& ev);
    void onRawJoystickInput(int iid, CaptureKind kind, int index, int value, bool deliberate);
    void closeAllDevices();

    void applySdlHints();
    bool initSdl();
    void onControllerAdded(const SDL_Event& ev);
    void onControllerRemoved(const SDL_Event& ev);
    void onJoystickAdded(const SDL_Event& ev);
    void onJoystickRemoved(const SDL_Event& ev);
    // A free record for a pad that just opened, or null when the table is full.
    Pad* claimPad(int iid, PadKind kind);
    void releasePad(Pad& pad);
    // Registers the pad with the processor and installs its default dead zones.
    void registerPad(Pad& pad);
    Pad* padFor(int iid);
    std::optional<std::size_t> slotOf(int iid) const;
    bool isRawJoystick(int iid) const;
    void markDirty(int iid);
    void rebuildDirty();
    // Takes the GUI's newest inputs, when it handed any over, into every open pad.
    void adoptInputs();
    void applyInputs(Pad& pad) const;
    // Hands the GUI a fresh device list and tells it so.
    void publishDevices();

    // Every open pad's mailbox, applied with SDL's calls (rumble, SetLED, SendEffect) on the SDL
    // thread. Called once per pump cycle.
    void drainMailboxes();
    void applyOutput(Pad& pad, const OutputMailbox::Taken& taken);
    // Build one effect kind's DualSense report and hand its body to SDL_GameControllerSendEffect.
    static void sendEffect(Pad& pad, std::size_t reportBytes,
                           const std::array<std::uint8_t, usbout::kMaxOutputReportBytes>& report);
    void rebuildState(Pad& pad);
    // RAW-joystick twin of rebuildState: reads an open SDL_Joystick's current
    // raw axis/button/hat state, runs the SDL-free JoystickMapping default
    // layout, and publishes the resulting report — the SAME processor path the
    // game-controller rebuildState uses. Only for pads SDL does NOT recognise
    // as game controllers.
    void rebuildJoystickState(Pad& pad);

    // Fixed caps, so the per-event read allocates nothing. A pad with more inputs than a cap is
    // truncated, which loses nothing because the layouts reference only low indices.
    static constexpr int kMaxJoystickAxes = 32;
    static constexpr int kMaxJoystickButtons = 64;
    static constexpr int kMaxJoystickHats = 8;
    static JoystickSnapshot readJoystick(SDL_Joystick* js, std::int16_t (&axes)[kMaxJoystickAxes],
                                         bool (&buttons)[kMaxJoystickButtons],
                                         std::uint8_t (&hats)[kMaxJoystickHats]);
    void handleSensorEvent(const SDL_ControllerSensorEvent& ev);
    void handleTouchpadEvent(const SDL_ControllerTouchpadEvent& ev);
    static void applyTouchFinger(TouchState& ts, const SDL_ControllerTouchpadEvent& ev);
    void pollBatteries(TimePoint now);
    void maybeEmitCapture(int iid, int kind, int index, int value);

    GamepadInputProcessor* processor_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    pump::PumpPolicy policy_{};

    // SDL thread only.
    std::array<Pad, kMaxPads> pads_{};
    DirtySet<kMaxPads> dirty_;
    int openPads_ = 0;
    TimePoint lastBatterySweep_{};
    std::unique_ptr<const Inputs> inputs_ = std::make_unique<const Inputs>();

    // The device list, from the SDL thread to devices(); readers keep the latest under devicesMtx_.
    mutable thread::LatestValue<QList<Device>> devicesBox_;
    mutable thread::TrackedMutex devicesMtx_{"bridge.devices"};
    mutable QList<Device> devices_;

    // The GUI's inputs to the SDL thread: the setters edit their copy under inputsMtx_ and hand
    // the SDL thread an immutable one, which it adopts at the top of its next cycle.
    thread::TrackedMutex inputsMtx_{"bridge.inputs"};
    Inputs inputsDraft_;
    thread::LatestValue<const Inputs> inputsBox_;

    // Capture mode flag (see setJoystickCaptureEnabled). A relaxed atomic so the
    // JOY event cases can gate the emit with no lock when capture is off (the
    // overwhelmingly common case).
    std::atomic<bool> captureEnabled_{false};

    // One mailbox per processor device index: written by whoever routes feedback to the device,
    // taken by the SDL thread, which alone may call SDL on a pad it might also close.
    std::array<OutputMailbox, GamepadInputProcessor::kMaxDevices> mailboxes_;
};

} // namespace dish::input
