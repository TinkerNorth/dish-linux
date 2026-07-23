// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "SDLGamepadBridge.h"

#include "SdlMotionConvert.h"
#include "Util/HostBattery.h"

// The canonical SDL2 include form: sdl2.pc puts the SDL2/ dir itself on
// the (system) include path, so <SDL.h> resolves through the -isystem entry
// on every platform. <SDL2/SDL.h> would resolve via /usr/local/include on
// macOS as a USER path and leak -Wold-style-cast into SDL's own headers.
#include <SDL.h>

#include <QLoggingCategory>
#include <QMetaObject>

#include <chrono>
#include <cstdint>

namespace dish::input {

namespace {

Q_LOGGING_CATEGORY(lcDishInput, "dish.input")

// Conservative noise-floor defaults applied to every newly-attached controller.
// ~10 % of the int16 stick range and ~5 % of the 0..255 trigger range. Mirrors
// the per-device flat values Android pulls out of
// `InputDevice.getMotionRange(axis).getFlat()`. SDL2 has no equivalent.
constexpr std::int16_t kDefaultStickFlat = 3277;
constexpr std::uint8_t kDefaultTriggerFlat = 13;

// The SDL → wire conversion helpers (gyroRadPerSecToInt16, accelMps2ToInt16,
// touchpadCoordToInt16) now live in SdlMotionConvert.{h,cpp} so they can be
// unit-tested without SDL/Qt. They are used unqualified below — both this TU
// and the helpers are in namespace dish::input.

// Map SDL2's joystick power level to the satellite battery wire constants
// + a coarse percent. SDL doesn't expose a continuous level on Windows
// (XInput gamepads only report EMPTY/LOW/MEDIUM/FULL); HID DualSense and
// some 8BitDo pads return UNKNOWN with charging info routed via separate
// hidraw paths. Bucket SDL's coarse enum to (level, status) that the
// satellite + ViGEm DS4 backend understands.
//
// EMPTY / LOW / MEDIUM / FULL are real wireless-pad readings — forwarded as
// the controller's own battery. WIRED (a USB pad) and UNKNOWN (no usable
// reading) carry no meaningful controller charge, so we substitute the HOST
// machine's battery instead via util::readHostBattery(): the laptop's own
// percentage + charging state, or 100 % / WIRED on a battery-less desktop.
// The (level, status) type is util::BatteryReading — shared with the
// HostBattery helper so the two code paths produce one wire shape.
using dish::util::BatteryReading;
using dish::util::kBatteryStatusDischarging;

BatteryReading powerLevelToWire(SDL_JoystickPowerLevel pl) {
    switch (pl) {
    case SDL_JOYSTICK_POWER_EMPTY:
        return {5, kBatteryStatusDischarging};
    case SDL_JOYSTICK_POWER_LOW:
        return {25, kBatteryStatusDischarging};
    case SDL_JOYSTICK_POWER_MEDIUM:
        return {60, kBatteryStatusDischarging};
    case SDL_JOYSTICK_POWER_FULL:
        return {100, kBatteryStatusDischarging};
    case SDL_JOYSTICK_POWER_WIRED:
    case SDL_JOYSTICK_POWER_UNKNOWN:
    default:
        // No usable controller reading — fall back to the host battery.
        return dish::util::readHostBattery();
    }
}

constexpr std::chrono::seconds kBatteryPollInterval{30};

// SDL_GameController axes are int16 [-32768, 32767]; pass through directly.
std::int16_t axisValue(SDL_GameController* gc, SDL_GameControllerAxis axis) {
    return SDL_GameControllerGetAxis(gc, axis);
}

std::uint8_t triggerValue(SDL_GameController* gc, SDL_GameControllerAxis axis) {
    // Triggers are 0..32767 on SDL2; scale to 0..255.
    const int v = SDL_GameControllerGetAxis(gc, axis);
    if (v <= 0) { return 0; }
    return static_cast<std::uint8_t>((v * 255) / 32767);
}

bool buttonDown(SDL_GameController* gc, SDL_GameControllerButton b) {
    return SDL_GameControllerGetButton(gc, b) != 0;
}

} // namespace

SDLGamepadBridge::SDLGamepadBridge(GamepadInputProcessor* processor, QObject* parent)
    : QObject(parent), processor_(processor) {}

SDLGamepadBridge::~SDLGamepadBridge() { stop(); }

void SDLGamepadBridge::start() {
    if (running_.exchange(true)) { return; }
    thread_ = std::thread([this] { runLoop(); });
}

void SDLGamepadBridge::stop() {
    if (!running_.exchange(false)) { return; }
    if (thread_.joinable()) { thread_.join(); }
}

QList<SDLGamepadBridge::Device> SDLGamepadBridge::devices() const {
    std::lock_guard<std::mutex> lock(mtx_);
    QList<Device> out;
    out.reserve(static_cast<int>(deviceIds_.size()));
    for (const auto& [iid, did] : deviceIds_) {
        Device dev{did,
                   deviceNames_.at(iid),
                   motionCapable_.count(iid) != 0,
                   lightbarCapable_.count(iid) != 0,
                   0xFF,
                   0};
        if (auto it = lastBattery_.find(iid); it != lastBattery_.end()) {
            dev.batteryLevel = it->second.level;
            dev.batteryStatus = it->second.status;
        }
        out.append(dev);
    }
    return out;
}

void SDLGamepadBridge::runLoop() {
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK) != 0) {
        running_.store(false);
        return;
    }
    SDL_GameControllerEventState(SDL_ENABLE);

    while (running_.load(std::memory_order_relaxed)) {
        SDL_Event ev;
        if (SDL_WaitEventTimeout(&ev, 100) == 0) { continue; }
        switch (ev.type) {
        case SDL_CONTROLLERDEVICEADDED: {
            SDL_GameController* gc = SDL_GameControllerOpen(ev.cdevice.which);
            if (gc == nullptr) { break; }
            SDL_Joystick* js = SDL_GameControllerGetJoystick(gc);
            const int iid = SDL_JoystickInstanceID(js);
            const auto* name = SDL_GameControllerName(gc);
            const QString deviceId = QStringLiteral("sdl:%1").arg(iid);
            const QString deviceName = QString::fromUtf8(name != nullptr ? name : "Gamepad");

            // Best-effort sensor enable. SDL_GameControllerHasSensor returns
            // SDL_TRUE for DualSense / DS4 / Switch Pro / Joy-Con; it returns
            // SDL_FALSE for Xbox 360 / Xbox One controllers which have no
            // IMU. The enable call still returns 0 (success) when the device
            // doesn't have the sensor — we re-check Has and only mark the
            // device as motion-capable when both calls agree.
            bool hasGyro = SDL_GameControllerHasSensor(gc, SDL_SENSOR_GYRO) == SDL_TRUE;
            bool hasAccel = SDL_GameControllerHasSensor(gc, SDL_SENSOR_ACCEL) == SDL_TRUE;
            if (hasGyro) {
                if (SDL_GameControllerSetSensorEnabled(gc, SDL_SENSOR_GYRO, SDL_TRUE) != 0) {
                    hasGyro = false;
                }
            }
            if (hasAccel) {
                if (SDL_GameControllerSetSensorEnabled(gc, SDL_SENSOR_ACCEL, SDL_TRUE) != 0) {
                    hasAccel = false;
                }
            }
            // Addressable RGB LED probe. SDL_GameControllerHasLED returns
            // SDL_TRUE for DualSense / DualShock 4 (and a handful of LED-bearing
            // third-party pads); SDL_FALSE for Xbox / Switch Pro / generic
            // pads. Drives the per-controller CAP_LIGHTBAR advertisement.
            const bool hasLed = SDL_GameControllerHasLED(gc) == SDL_TRUE;
            // SDL's negotiated type — diagnostics only (the emulated type is now
            // catalog-driven, not derived from the physical pad).
            const auto type = SDL_GameControllerGetType(gc);
            {
                std::lock_guard<std::mutex> lock(mtx_);
                openControllers_[iid] = gc;
                deviceIds_[iid] = deviceId;
                deviceNames_[iid] = deviceName;
                if (hasGyro || hasAccel) { motionCapable_.insert(iid); }
                if (hasLed) { lightbarCapable_.insert(iid); }
                lastBatteryPoll_[iid] = std::chrono::steady_clock::time_point{};
            }
            // One-shot device-capability dump — mirrors the SatelliteJNI
            // DEVCAPS log on Android (PR #44/#47). SDL reports the controller
            // type it negotiated (Xbox 360 / DualSense / generic), the vendor
            // / product id, and the GUID; together that pins what mapping was
            // applied so users reporting "my pad doesn't work" get a usable
            // diagnostic without a debugger.
            const auto vid = SDL_GameControllerGetVendor(gc);
            const auto pid = SDL_GameControllerGetProduct(gc);
            char guidBuf[64] = {0};
            SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(js), guidBuf, sizeof(guidBuf));
            qCInfo(lcDishInput) << "DEVCAPS id=" << deviceId << "name=" << deviceName
                                << "type=" << static_cast<int>(type)
                                << "vid=" << QString::number(vid, 16)
                                << "pid=" << QString::number(pid, 16) << "guid=" << guidBuf
                                << "gyro=" << hasGyro << "accel=" << hasAccel << "led=" << hasLed;
            // Push the default deadzone profile so the processor filters
            // out controller noise from the first event. The default lives
            // inside the bridge (not the processor) because the bridge is
            // the only thing that knows when a device shows up.
            processor_->setDeadzones(deviceId.toStdString(),
                                     {kDefaultStickFlat, kDefaultTriggerFlat});
            QMetaObject::invokeMethod(this, "devicesChanged", Qt::QueuedConnection);
            rebuildState(iid);
            break;
        }
        case SDL_CONTROLLERDEVICEREMOVED: {
            const int iid = ev.cdevice.which;
            std::string deviceId;
            {
                std::lock_guard<std::mutex> lock(mtx_);
                if (auto it = openControllers_.find(iid); it != openControllers_.end()) {
                    SDL_GameControllerClose(it->second);
                    openControllers_.erase(it);
                }
                if (auto it = deviceIds_.find(iid); it != deviceIds_.end()) {
                    deviceId = it->second.toStdString();
                    deviceIds_.erase(it);
                }
                deviceNames_.erase(iid);
                motionCapable_.erase(iid);
                lightbarCapable_.erase(iid);
                lastBatteryPoll_.erase(iid);
                lastBattery_.erase(iid);
                touchState_.erase(iid);
            }
            if (!deviceId.empty()) { processor_->remove(deviceId); }
            QMetaObject::invokeMethod(this, "devicesChanged", Qt::QueuedConnection);
            break;
        }
        case SDL_CONTROLLERAXISMOTION:
        case SDL_CONTROLLERBUTTONDOWN:
        case SDL_CONTROLLERBUTTONUP:
            rebuildState(ev.cdevice.which);
            break;
        case SDL_CONTROLLERSENSORUPDATE:
            handleSensorEvent(ev.csensor);
            break;
        case SDL_CONTROLLERTOUCHPADDOWN:
        case SDL_CONTROLLERTOUCHPADMOTION:
        case SDL_CONTROLLERTOUCHPADUP:
            handleTouchpadEvent(ev.ctouchpad);
            break;
        default:
            break;
        }

        // Execute any rumble / lightbar commands queued by applyRumble /
        // applyLightbar on the receive thread. Done here so every
        // SDL_GameController* is resolved and used only on the SDL thread.
        drainOutputCommands();

        // Poll battery on every event-loop iteration; the per-device gate
        // collapses to a 30 s cadence so this is cheap.
        pollBatteries();
    }

    {
        std::lock_guard<std::mutex> lock(mtx_);
        for (auto& [iid, gc] : openControllers_) { SDL_GameControllerClose(gc); }
        openControllers_.clear();
        deviceIds_.clear();
        deviceNames_.clear();
    }
    SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK);
}

void SDLGamepadBridge::applyRumble(const QString& deviceId, std::uint16_t strongMagnitude,
                                   std::uint16_t weakMagnitude, std::uint16_t durationMs) {
    // Queue the rumble for the SDL thread — see outputQueue_'s comment in the
    // header. Vibration only: the lightbar is handled by applyLightbar.
    outputQueue_.push(OutputCommand::rumble(deviceId, strongMagnitude, weakMagnitude, durationMs));
}

void SDLGamepadBridge::handleSensorEvent(const SDL_ControllerSensorEvent& ev) {
    // Axis-orientation assumption (load-bearing): SDL2 delivers gyro/accel
    // for HIDAPI controllers (DualSense / DS4 / Switch Pro) already rotated
    // into a right-handed +X-right / +Y-up / +Z-toward-player frame — the
    // exact convention the satellite wire format wants (protocol.md §0x000A).
    // We therefore feed ev.data straight through the scale-only converters
    // below; we deliberately do NOT apply a manufacturer rotation matrix,
    // because SDL has already done it and a second rotation would corrupt
    // the frame. See SatelliteClient::sendMotion for the matching note.
    //
    // SDL maps the joystick instance id into `which`. Resolve to deviceId
    // under the same mutex that protects deviceIds_ — handleSensorEvent
    // is called only from the input thread but `devices()` / `applyRumble`
    // can be called from elsewhere.
    std::string deviceId;
    AccelCache accel{};
    bool motionCap = false;
    bool isGyro = (ev.sensor == SDL_SENSOR_GYRO);
    bool isAccel = (ev.sensor == SDL_SENSOR_ACCEL);
    if (!isGyro && !isAccel) { return; }

    {
        std::lock_guard<std::mutex> lock(mtx_);
        const int iid = ev.which;
        motionCap = motionCapable_.count(iid) != 0;
        if (!motionCap) { return; }
        if (auto it = deviceIds_.find(iid); it != deviceIds_.end()) {
            deviceId = it->second.toStdString();
        }
        if (deviceId.empty()) { return; }
        if (isAccel) {
            AccelCache c{ev.data[0], ev.data[1], ev.data[2]};
            lastAccel_[iid] = c;
            // Accel-only updates piggy-back on the next gyro event so that
            // the rate-limiter sees one stream, not two. Returning here
            // means accel-only emitters (rare; e.g. some Joy-Con SR/SL
            // configs) won't drive MSG_MOTION on their own. Acceptable for
            // a first cut — gyro-less pads aren't useful for gyro aim.
            return;
        }
        auto accelIt = lastAccel_.find(iid);
        if (accelIt == lastAccel_.end()) {
            // Gyro arrived before the first accelerometer sample for this
            // device. Emitting now would ship an accel triple of {0,0,0},
            // which the satellite would interpret as the pad in free-fall.
            // Drop this gyro event; the next one (after a cached accel) is
            // < 4 ms away on every IMU pad we support.
            return;
        }
        accel = accelIt->second;
    }

    GamepadInputProcessor::MotionSample sample{};
    sample.gyroX = gyroRadPerSecToInt16(ev.data[0]);
    sample.gyroY = gyroRadPerSecToInt16(ev.data[1]);
    sample.gyroZ = gyroRadPerSecToInt16(ev.data[2]);
    sample.accelX = accelMps2ToInt16(accel.ax);
    sample.accelY = accelMps2ToInt16(accel.ay);
    sample.accelZ = accelMps2ToInt16(accel.az);

    processor_->publishMotion(deviceId, sample);
}

void SDLGamepadBridge::handleTouchpadEvent(const SDL_ControllerTouchpadEvent& ev) {
    std::string deviceId;
    SDL_GameController* gc = nullptr;
    TouchState state;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        const int iid = ev.which;
        if (auto it = deviceIds_.find(iid); it != deviceIds_.end()) {
            deviceId = it->second.toStdString();
        }
        if (deviceId.empty()) { return; }
        if (auto it = openControllers_.find(iid); it != openControllers_.end()) { gc = it->second; }
        // SDL's `finger` is the 0-based slot within the touchpad. The DS4 /
        // DualSense pad tracks up to two simultaneous contacts; ignore any
        // higher slot index defensively.
        TouchState& ts = touchState_[iid];
        if (ev.finger >= 0 && ev.finger < 2) {
            TouchFinger& f = ts.fingers[ev.finger];
            if (ev.type == SDL_CONTROLLERTOUCHPADUP) {
                f.active = false;
            } else {
                // A TOUCHPADDOWN that finds the slot inactive is a fresh
                // contact (false→true edge): bump the monotonic tracking id
                // so the receiver sees a new touch rather than a continuation
                // of the previous one. It wraps freely as a uint8_t. A
                // TOUCHPADMOTION (or a redundant DOWN on an already-active
                // slot) keeps the id, marking the same continuing contact.
                if (ev.type == SDL_CONTROLLERTOUCHPADDOWN && !f.active) { ++f.id; }
                f.active = true;
                f.x = touchpadCoordToInt16(ev.x);
                f.y = touchpadCoordToInt16(ev.y);
            }
        }
        state = ts;
    }

    // The clickable-pad switch is exposed as an ordinary SDL button; read its
    // live state rather than tracking it separately.
    bool button = false;
    if (gc != nullptr) {
        button = SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_TOUCHPAD) == 1;
    }

    GamepadInputProcessor::TouchpadSample sample{};
    sample.finger0Active = state.fingers[0].active;
    sample.finger0Id = state.fingers[0].id;
    sample.finger0X = state.fingers[0].x;
    sample.finger0Y = state.fingers[0].y;
    sample.finger1Active = state.fingers[1].active;
    sample.finger1Id = state.fingers[1].id;
    sample.finger1X = state.fingers[1].x;
    sample.finger1Y = state.fingers[1].y;
    sample.buttonPressed = button;
    // SDL stamps controller events with uptime ms — exactly the u32 the wire
    // wants; resends of the same state carry the same value.
    sample.eventTimeMs = ev.timestamp;
    processor_->publishTouchpad(deviceId, sample);
}

void SDLGamepadBridge::pollBatteries() {
    const auto now = std::chrono::steady_clock::now();
    // Snapshot the (iid → deviceId) map plus per-device last-poll, then
    // iterate without holding the mutex so the SDL calls stay outside the
    // critical section. SDL_JoystickCurrentPowerLevel is cheap and
    // thread-safe, but holding mtx_ across the publish would block
    // applyRumble unnecessarily.
    struct PollEntry {
        int iid;
        std::string deviceId;
        SDL_GameController* gc;
    };
    std::vector<PollEntry> due;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        due.reserve(openControllers_.size());
        for (const auto& [iid, gc] : openControllers_) {
            const auto last = lastBatteryPoll_[iid];
            const bool first = last == std::chrono::steady_clock::time_point{};
            if (!first && (now - last) < kBatteryPollInterval) { continue; }
            lastBatteryPoll_[iid] = now;
            std::string did;
            if (auto it = deviceIds_.find(iid); it != deviceIds_.end()) {
                did = it->second.toStdString();
            }
            if (did.empty()) { continue; }
            due.push_back({iid, std::move(did), gc});
        }
    }

    bool anyChange = false;
    for (const auto& e : due) {
        SDL_Joystick* js = SDL_GameControllerGetJoystick(e.gc);
        if (js == nullptr) { continue; }
        const auto pl = SDL_JoystickCurrentPowerLevel(js);
        const auto wire = powerLevelToWire(pl);
        // Forward every poll. MSG_BATTERY is a fixed 30 s heartbeat: the
        // receiver expects a packet each interval even when the value is
        // unchanged, so a lost UDP packet self-heals on the next tick. The
        // 30 s gate above is the cadence; publishBattery does not coalesce.
        GamepadInputProcessor::BatterySample sample{wire.level, wire.status};
        processor_->publishBattery(e.deviceId, sample);
        // Record the sample for the UI battery chip. Note whether it changed
        // so we only nudge the UI to re-render on an actual transition.
        std::lock_guard<std::mutex> lock(mtx_);
        BatterySnapshot& snap = lastBattery_[e.iid];
        if (snap.level != wire.level || snap.status != wire.status) {
            snap.level = wire.level;
            snap.status = wire.status;
            anyChange = true;
        }
    }
    // A changed battery sample means the SlotCard chip is stale; ask the UI
    // to rebuild off devices() on the Qt main thread. Coalesced to one signal
    // per poll batch — the 30 s gate already keeps this rare.
    if (anyChange) { QMetaObject::invokeMethod(this, "devicesChanged", Qt::QueuedConnection); }
}

void SDLGamepadBridge::applyLightbar(const QString& deviceId, std::uint8_t r, std::uint8_t g,
                                     std::uint8_t b) {
    // Queue the colour for the SDL thread — see outputQueue_'s comment in the
    // header. The actual SDL_GameControllerSetLED runs in drainOutputCommands()
    // so a controller closed on the SDL thread between now and then is skipped
    // rather than used after free.
    outputQueue_.push(OutputCommand::lightbar(deviceId, r, g, b));
}

void SDLGamepadBridge::drainOutputCommands() {
    // Runs on the SDL thread. drain() atomically takes the batch so the
    // receive thread can keep enqueueing while we execute it.
    for (const auto& cmd : outputQueue_.drain()) {
        // Resolve the device id → SDL_GameController* here, on the SDL thread.
        // A controller removed since the command was enqueued is absent from
        // openControllers_, so gc stays null and the command is dropped — no
        // use-after-close. mtx_ guards the device map.
        SDL_GameController* gc = nullptr;
        {
            std::lock_guard<std::mutex> lock(mtx_);
            for (const auto& [iid, did] : deviceIds_) {
                if (did == cmd.deviceId) {
                    if (auto it = openControllers_.find(iid); it != openControllers_.end()) {
                        gc = it->second;
                    }
                    break;
                }
            }
        }
        if (gc == nullptr) { continue; }
        if (cmd.kind == OutputKind::Rumble) {
            // SDL2's SDL_GameControllerRumble returns 0 on success, -1 if the
            // device doesn't support rumble — silent: the caller has no
            // recourse, and the satellite-side game doesn't know either way.
            SDL_GameControllerRumble(gc, cmd.strongMagnitude, cmd.weakMagnitude, cmd.durationMs);
        } else {
            // SDL_GameControllerSetLED is a no-op on pads without a lightbar;
            // the failure is not surfaced for the same reason rumble's isn't.
            SDL_GameControllerSetLED(gc, cmd.r, cmd.g, cmd.b);
        }
    }
}

void SDLGamepadBridge::rebuildState(int iid) {
    SDL_GameController* gc = nullptr;
    std::string deviceId;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        if (auto it = openControllers_.find(iid); it != openControllers_.end()) { gc = it->second; }
        if (auto it = deviceIds_.find(iid); it != deviceIds_.end()) {
            deviceId = it->second.toStdString();
        }
    }
    if (gc == nullptr || deviceId.empty()) { return; }

    GamepadInputProcessor::DeviceState st{};
    using B = GamepadInputProcessor::Buttons;
    std::uint16_t btn = 0;
    if (buttonDown(gc, SDL_CONTROLLER_BUTTON_DPAD_UP)) btn |= B::kDpadUp;
    if (buttonDown(gc, SDL_CONTROLLER_BUTTON_DPAD_DOWN)) btn |= B::kDpadDown;
    if (buttonDown(gc, SDL_CONTROLLER_BUTTON_DPAD_LEFT)) btn |= B::kDpadLeft;
    if (buttonDown(gc, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) btn |= B::kDpadRight;
    if (buttonDown(gc, SDL_CONTROLLER_BUTTON_START)) btn |= B::kStart;
    if (buttonDown(gc, SDL_CONTROLLER_BUTTON_BACK)) btn |= B::kBack;
    if (buttonDown(gc, SDL_CONTROLLER_BUTTON_LEFTSTICK)) btn |= B::kLeftThumb;
    if (buttonDown(gc, SDL_CONTROLLER_BUTTON_RIGHTSTICK)) btn |= B::kRightThumb;
    if (buttonDown(gc, SDL_CONTROLLER_BUTTON_LEFTSHOULDER)) btn |= B::kLeftShoulder;
    if (buttonDown(gc, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) btn |= B::kRightShoulder;
    if (buttonDown(gc, SDL_CONTROLLER_BUTTON_A)) btn |= B::kA;
    if (buttonDown(gc, SDL_CONTROLLER_BUTTON_B)) btn |= B::kB;
    if (buttonDown(gc, SDL_CONTROLLER_BUTTON_X)) btn |= B::kX;
    if (buttonDown(gc, SDL_CONTROLLER_BUTTON_Y)) btn |= B::kY;
    st.wButtons = btn;
    st.lt = triggerValue(gc, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
    st.rt = triggerValue(gc, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
    st.lx = axisValue(gc, SDL_CONTROLLER_AXIS_LEFTX);
    // SDL Y axis is +down; XUSB expects +up. Invert.
    st.ly = static_cast<std::int16_t>(-axisValue(gc, SDL_CONTROLLER_AXIS_LEFTY));
    st.rx = axisValue(gc, SDL_CONTROLLER_AXIS_RIGHTX);
    st.ry = static_cast<std::int16_t>(-axisValue(gc, SDL_CONTROLLER_AXIS_RIGHTY));

    processor_->publish(deviceId, st);
}

} // namespace dish::input
