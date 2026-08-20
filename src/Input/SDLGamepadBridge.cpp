// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "SDLGamepadBridge.h"

#include "core/input/HidTransport.h"
#include "JoystickMapping.h"
#include "SdlMotionConvert.h"
#include "Util/HostBattery.h"

#include <SDL2/SDL.h>

#include <QLoggingCategory>
#include <QMetaObject>

#include <chrono>
#include <cstdint>

namespace dish::input {

namespace {

Q_LOGGING_CATEGORY(lcDishInput, "dish.input")

// Noise floor for every newly-attached controller: ~10 % of the int16 stick
// range and ~5 % of the 0..255 trigger range. Fixed values because SDL2 exposes
// no per-device flat/fuzz the way the Android input framework does.
constexpr std::int16_t kDefaultStickFlat = 3277;
constexpr std::uint8_t kDefaultTriggerFlat = 13;

// The satellite's own ViGEm pad enumerates as "Dish ...". Skipping it as an input
// stops a single-machine setup looping the emulated output back in. No real
// controller carries this name.
bool isDishVirtualDevice(const char* name) {
    return name != nullptr && QString::fromUtf8(name).startsWith(QStringLiteral("Dish "));
}

// SDL exposes no continuous battery level here: pads report only
// EMPTY/LOW/MEDIUM/FULL, and HID DualSense and some 8BitDo pads report UNKNOWN
// because their charging info rides separate hidraw paths. The four real
// readings become a coarse percent; WIRED and UNKNOWN carry no controller charge
// at all, so the host machine's battery is substituted rather than shipping a
// fake number.
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
        return dish::util::readHostBattery();
    }
}

constexpr std::chrono::seconds kBatteryPollInterval{30};

// SDL axes are already the int16 the wire wants.
std::int16_t axisValue(SDL_GameController* gc, SDL_GameControllerAxis axis) {
    return SDL_GameControllerGetAxis(gc, axis);
}

std::uint8_t triggerValue(SDL_GameController* gc, SDL_GameControllerAxis axis) {
    // SDL2 triggers are 0..32767, the wire wants 0..255.
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
        if (auto it = usbIdentity_.find(iid); it != usbIdentity_.end()) {
            dev.vendorId = it->second.vendorId;
            dev.productId = it->second.productId;
        }
        dev.isRawJoystick = openJoysticks_.count(iid) != 0;
        dev.hasTouchpad = touchpadCapable_.count(iid) != 0;
        dev.hasRumble = rumbleCapable_.count(iid) != 0;
        dev.bluetooth = bluetoothIids_.count(iid) != 0;
        out.append(dev);
    }
    return out;
}

void SDLGamepadBridge::runLoop() {
    // Positional (Xbox-layout) buttons, not label-based, because the USB-direct
    // decoders map by physical position. Without this hint a Switch Pro would
    // disagree with itself across the SDL and Direct paths.
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "0");
    // Qt owns this process's lifecycle. Left to itself SDL installs SIGINT and
    // SIGTERM handlers that raise SDL_QUIT, which nothing here consumes — so
    // the app ignored SIGTERM entirely and a logout sat through systemd's full
    // 90 s wait before the SIGKILL. Packaging CI is what caught it.
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK) != 0) {
        running_.store(false);
        return;
    }
    SDL_GameControllerEventState(SDL_ENABLE);
    // Joystick event delivery is a separate opt-in even though SDL_INIT_JOYSTICK
    // is already up. Needed so generic pads SDL does not recognise as game
    // controllers still surface.
    SDL_JoystickEventState(SDL_ENABLE);

    while (running_.load(std::memory_order_relaxed)) {
        SDL_Event ev;
        if (SDL_WaitEventTimeout(&ev, 100) == 0) { continue; }
        switch (ev.type) {
        case SDL_CONTROLLERDEVICEADDED: {
            if (isDishVirtualDevice(SDL_GameControllerNameForIndex(ev.cdevice.which))) { break; }
            SDL_GameController* gc = SDL_GameControllerOpen(ev.cdevice.which);
            if (gc == nullptr) { break; }
            SDL_Joystick* js = SDL_GameControllerGetJoystick(gc);
            const int iid = SDL_JoystickInstanceID(js);
            const auto* name = SDL_GameControllerName(gc);
            const QString deviceId = QStringLiteral("sdl:%1").arg(iid);
            const QString deviceName = QString::fromUtf8(name != nullptr ? name : "Gamepad");

            // SDL's enable call returns success even for a device with no such
            // sensor, so Has and Set must both agree before marking it capable.
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
            const bool hasLed = SDL_GameControllerHasLED(gc) == SDL_TRUE;
            const bool hasTouchpad = SDL_GameControllerGetNumTouchpads(gc) > 0;
            const bool hasRumble = SDL_GameControllerHasRumble(gc) == SDL_TRUE;
            const auto type = SDL_GameControllerGetType(gc); // DEVCAPS log only
            // SDL returns 0 when it cannot read the descriptor, which the
            // twin-dedup pairing treats as "no identity".
            const int vendorId = SDL_GameControllerGetVendor(gc);
            const int productId = SDL_GameControllerGetProduct(gc);
            // Resolved through sysfs, so the answer is the HID bus rather than
            // a naming convention. A null path reads as not-Bluetooth, which
            // fails safe to the wired presentation.
            const char* devPath = SDL_GameControllerPath(gc);
            const bool bluetooth =
                devPath != nullptr && dish::input::isBluetoothHidDevicePath(devPath);
            {
                std::lock_guard<std::mutex> lock(mtx_);
                openControllers_[iid] = gc;
                deviceIds_[iid] = deviceId;
                deviceNames_[iid] = deviceName;
                if (hasGyro || hasAccel) { motionCapable_.insert(iid); }
                if (hasLed) { lightbarCapable_.insert(iid); }
                if (hasTouchpad) { touchpadCapable_.insert(iid); }
                if (hasRumble) { rumbleCapable_.insert(iid); }
                if (bluetooth) { bluetoothIids_.insert(iid); }
                usbIdentity_[iid] = {vendorId, productId};
                lastBatteryPoll_[iid] = std::chrono::steady_clock::time_point{};
            }
            // One-shot capability dump: type, ids and GUID together pin which
            // mapping SDL applied, so a "my pad doesn't work" report is
            // diagnosable without a debugger.
            char guidBuf[64] = {0};
            SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(js), guidBuf, sizeof(guidBuf));
            qCInfo(lcDishInput) << "DEVCAPS id=" << deviceId << "name=" << deviceName
                                << "type=" << static_cast<int>(type)
                                << "vid=" << QString::number(vendorId, 16)
                                << "pid=" << QString::number(productId, 16) << "guid=" << guidBuf
                                << "gyro=" << hasGyro << "accel=" << hasAccel << "led=" << hasLed
                                << "rumble=" << hasRumble << "bt=" << bluetooth;
            // Pushed from here rather than owned by the processor because the
            // bridge is the only thing that knows when a device shows up.
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
                touchpadCapable_.erase(iid);
                rumbleCapable_.erase(iid);
                bluetoothIids_.erase(iid);
                usbIdentity_.erase(iid);
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
        case SDL_JOYDEVICEADDED: {
            // Unlike every other event here, this `which` is a device INDEX, not
            // an instance id — it matches SDL_JoystickOpen's argument.
            const int which = ev.jdevice.which;
            // A game controller ALSO emits joystick events, and the controller
            // path owns recognised pads. Opening one here would double it.
            if (SDL_IsGameController(which) == SDL_TRUE) { break; }
            if (isDishVirtualDevice(SDL_JoystickNameForIndex(which))) { break; }
            SDL_Joystick* js = SDL_JoystickOpen(which);
            if (js == nullptr) { break; }
            const int iid = SDL_JoystickInstanceID(js);
            const auto* name = SDL_JoystickName(js);
            const QString deviceId = QStringLiteral("sdl:%1").arg(iid);
            const QString deviceName = QString::fromUtf8(name != nullptr ? name : "Joystick");
            // SDL's joystick API exposes no IMU, LED or controller type, so a raw
            // joystick surfaces as a plain Xbox-kind pad with neither.
            const int vendorId = SDL_JoystickGetVendor(js);
            const int productId = SDL_JoystickGetProduct(js);
            const char* devPath = SDL_JoystickPath(js);
            const bool bluetooth =
                devPath != nullptr && dish::input::isBluetoothHidDevicePath(devPath);
            const bool hasRumble = SDL_JoystickHasRumble(js) == SDL_TRUE;
            {
                std::lock_guard<std::mutex> lock(mtx_);
                openJoysticks_[iid] = js;
                deviceIds_[iid] = deviceId;
                deviceNames_[iid] = deviceName;
                if (hasRumble) { rumbleCapable_.insert(iid); }
                if (bluetooth) { bluetoothIids_.insert(iid); }
                usbIdentity_[iid] = {vendorId, productId};
                lastBatteryPoll_[iid] = std::chrono::steady_clock::time_point{};
            }
            char guidBuf[64] = {0};
            SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(js), guidBuf, sizeof(guidBuf));
            qCInfo(lcDishInput) << "DEVCAPS(joystick) id=" << deviceId << "name=" << deviceName
                                << "axes=" << SDL_JoystickNumAxes(js)
                                << "buttons=" << SDL_JoystickNumButtons(js)
                                << "hats=" << SDL_JoystickNumHats(js)
                                << "vid=" << QString::number(vendorId, 16)
                                << "pid=" << QString::number(productId, 16) << "guid=" << guidBuf
                                << "bt=" << bluetooth;
            processor_->setDeadzones(deviceId.toStdString(),
                                     {kDefaultStickFlat, kDefaultTriggerFlat});
            QMetaObject::invokeMethod(this, "devicesChanged", Qt::QueuedConnection);
            rebuildJoystickState(iid);
            break;
        }
        case SDL_JOYDEVICEREMOVED: {
            const int iid = ev.jdevice.which;
            std::string deviceId;
            {
                std::lock_guard<std::mutex> lock(mtx_);
                // Absent for a controller-path device, so this only acts on raw
                // joysticks we actually opened.
                auto jit = openJoysticks_.find(iid);
                if (jit == openJoysticks_.end()) { break; }
                SDL_JoystickClose(jit->second);
                openJoysticks_.erase(jit);
                if (auto it = deviceIds_.find(iid); it != deviceIds_.end()) {
                    deviceId = it->second.toStdString();
                    deviceIds_.erase(it);
                }
                deviceNames_.erase(iid);
                rumbleCapable_.erase(iid);
                bluetoothIids_.erase(iid);
                usbIdentity_.erase(iid);
                lastBatteryPoll_.erase(iid);
                lastBattery_.erase(iid);
            }
            if (!deviceId.empty()) { processor_->remove(deviceId); }
            QMetaObject::invokeMethod(this, "devicesChanged", Qt::QueuedConnection);
            break;
        }
        case SDL_JOYAXISMOTION:
            // A game controller's joystick events also land here, but its iid is
            // in openControllers_ and never openJoysticks_, so this no-ops for it.
            rebuildJoystickState(ev.jaxis.which);
            // The magnitude gate rejects idle jitter, and the flag check keeps
            // capture free when off.
            if (captureEnabled_.load(std::memory_order_relaxed) &&
                captureAxisPasses(ev.jaxis.value)) {
                maybeEmitCapture(ev.jaxis.which, static_cast<int>(CaptureKind::Axis), ev.jaxis.axis,
                                 ev.jaxis.value);
            }
            break;
        case SDL_JOYBUTTONDOWN:
            rebuildJoystickState(ev.jbutton.which);
            // Press only; a release is not an assignment.
            if (captureEnabled_.load(std::memory_order_relaxed) && captureButtonPasses()) {
                maybeEmitCapture(ev.jbutton.which, static_cast<int>(CaptureKind::Button),
                                 ev.jbutton.button, 1);
            }
            break;
        case SDL_JOYBUTTONUP:
            rebuildJoystickState(ev.jbutton.which);
            break;
        case SDL_JOYHATMOTION:
            rebuildJoystickState(ev.jhat.which);
            // Non-centered directions only, for the same reason.
            if (captureEnabled_.load(std::memory_order_relaxed) &&
                captureHatPasses(ev.jhat.value)) {
                maybeEmitCapture(ev.jhat.which, static_cast<int>(CaptureKind::Hat), ev.jhat.hat,
                                 ev.jhat.value);
            }
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

        // Here, so every SDL_GameController* is resolved and used only on the SDL
        // thread.
        drainOutputCommands();

        // Cheap despite running every iteration: the per-device gate inside
        // collapses it to a 30 s cadence.
        pollBatteries();
    }

    {
        std::lock_guard<std::mutex> lock(mtx_);
        for (auto& [iid, gc] : openControllers_) { SDL_GameControllerClose(gc); }
        openControllers_.clear();
        for (auto& [iid, js] : openJoysticks_) { SDL_JoystickClose(js); }
        openJoysticks_.clear();
        deviceIds_.clear();
        deviceNames_.clear();
    }
    SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK);
}

void SDLGamepadBridge::applyRumble(const QString& deviceId, std::uint16_t strongMagnitude,
                                   std::uint16_t weakMagnitude, std::uint16_t durationMs) {
    outputQueue_.push(OutputCommand::rumble(deviceId, strongMagnitude, weakMagnitude, durationMs));
}

void SDLGamepadBridge::setSuppressedDeviceIds(const std::unordered_set<std::string>& ids) {
    std::lock_guard<std::mutex> lock(suppressedMtx_);
    suppressedIds_ = ids;
}

bool SDLGamepadBridge::isSuppressed(const std::string& deviceId) const {
    std::lock_guard<std::mutex> lock(suppressedMtx_);
    return suppressedIds_.count(deviceId) != 0;
}

void SDLGamepadBridge::setJoystickRemap(int vendorId, int productId, const JoystickRemap& remap) {
    std::lock_guard<std::mutex> lock(remapMtx_);
    joystickRemaps_[{vendorId, productId}] = remap;
}

void SDLGamepadBridge::clearJoystickRemap(int vendorId, int productId) {
    std::lock_guard<std::mutex> lock(remapMtx_);
    joystickRemaps_.erase({vendorId, productId});
}

void SDLGamepadBridge::setJoystickCaptureEnabled(bool enabled) {
    captureEnabled_.store(enabled, std::memory_order_relaxed);
}

void SDLGamepadBridge::maybeEmitCapture(int iid, int kind, int index, int value) {
    // Re-checked despite the callers' guard, so this is a safe no-op if ever
    // called unguarded.
    if (!captureEnabled_.load(std::memory_order_relaxed)) { return; }
    QString deviceId;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        if (auto it = deviceIds_.find(iid); it != deviceIds_.end()) { deviceId = it->second; }
    }
    if (deviceId.isEmpty()) { return; }
    // QueuedConnection hops SDL thread to GUI thread; the args are value types,
    // so the cross-thread copy is safe.
    QMetaObject::invokeMethod(this, "rawJoystickInput", Qt::QueuedConnection,
                              Q_ARG(QString, deviceId), Q_ARG(int, kind), Q_ARG(int, index),
                              Q_ARG(int, value));
}

void SDLGamepadBridge::handleSensorEvent(const SDL_ControllerSensorEvent& ev) {
    // Deliberately no rotation here: SDL applies the per-model matrix internally
    // for HIDAPI controllers, so samples already arrive in the satellite's
    // right-handed frame and only the units need converting.
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
            // Accel piggy-backs on the next gyro event so the rate limiter sees
            // one stream, not two. A gyro-less pad therefore never drives
            // MSG_MOTION, which is fine since it cannot do gyro aim anyway.
            return;
        }
        // Skip until an accel sample exists: publishing accel{0,0,0} would ship a
        // spurious zero-gravity triple.
        auto accelIt = lastAccel_.find(iid);
        if (accelIt == lastAccel_.end()) { return; }
        accel = accelIt->second;
    }

    // Twin-dedup: suppress motion too while USB-direct owns this pad.
    if (isSuppressed(deviceId)) { return; }

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
        // `finger` is the 0-based slot; the wire carries only two, so anything
        // higher is dropped.
        TouchState& ts = touchState_[iid];
        if (ev.finger >= 0 && ev.finger < 2) {
            TouchFinger& f = ts.fingers[ev.finger];
            if (ev.type == SDL_CONTROLLERTOUCHPADUP) {
                f.active = false;
            } else {
                // Only a false→true DOWN edge takes a new tracking id, so a
                // MOTION on an already-active finger keeps its id. The counter
                // wraps freely: the protocol needs the id to CHANGE on a new
                // contact, not to be globally unique.
                if (ev.type == SDL_CONTROLLERTOUCHPADDOWN && !f.active) { ++f.id; }
                f.active = true;
                f.x = touchpadCoordToInt16(ev.x);
                f.y = touchpadCoordToInt16(ev.y);
            }
        }
        state = ts;
    }

    // The clickable-pad switch is an ordinary SDL button, so read it live rather
    // than tracking it separately.
    bool button = false;
    if (gc != nullptr) {
        button = SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_TOUCHPAD) == 1;
    }

    // Twin-dedup: suppress the touchpad surface too while USB-direct owns the pad.
    if (isSuppressed(deviceId)) { return; }

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
    processor_->publishTouchpad(deviceId, sample);
}

void SDLGamepadBridge::pollBatteries() {
    const auto now = std::chrono::steady_clock::now();
    // Snapshot under the lock, then iterate outside it: holding mtx_ across the
    // publish would block applyRumble for no reason. Resolving the joystick
    // handle here lets the controller and raw-joystick paths share one poll.
    struct PollEntry {
        int iid;
        std::string deviceId;
        SDL_Joystick* js;
    };
    std::vector<PollEntry> due;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        due.reserve(openControllers_.size() + openJoysticks_.size());
        auto consider = [&](int iid, SDL_Joystick* js) {
            if (js == nullptr) { return; }
            const auto last = lastBatteryPoll_[iid];
            const bool first = last == std::chrono::steady_clock::time_point{};
            if (!first && (now - last) < kBatteryPollInterval) { return; }
            lastBatteryPoll_[iid] = now;
            std::string did;
            if (auto it = deviceIds_.find(iid); it != deviceIds_.end()) {
                did = it->second.toStdString();
            }
            if (did.empty()) { return; }
            due.push_back({iid, std::move(did), js});
        };
        for (const auto& [iid, gc] : openControllers_) {
            consider(iid, SDL_GameControllerGetJoystick(gc));
        }
        for (const auto& [iid, js] : openJoysticks_) { consider(iid, js); }
    }

    bool anyChange = false;
    for (const auto& e : due) {
        SDL_Joystick* js = e.js;
        if (js == nullptr) { continue; }
        const auto pl = SDL_JoystickCurrentPowerLevel(js);
        const auto wire = powerLevelToWire(pl);
        // Forwarded unconditionally: MSG_BATTERY is a 30 s heartbeat, so the
        // receiver expects a packet each interval even when the value is
        // unchanged, and a lost one self-heals on the next tick.
        GamepadInputProcessor::BatterySample sample{wire.level, wire.status};
        processor_->publishBattery(e.deviceId, sample);
        std::lock_guard<std::mutex> lock(mtx_);
        BatterySnapshot& snap = lastBattery_[e.iid];
        if (snap.level != wire.level || snap.status != wire.status) {
            snap.level = wire.level;
            snap.status = wire.status;
            anyChange = true;
        }
    }
    // One signal per batch, not per device, so the UI rebuilds once.
    if (anyChange) { QMetaObject::invokeMethod(this, "devicesChanged", Qt::QueuedConnection); }
}

void SDLGamepadBridge::applyLightbar(const QString& deviceId, std::uint8_t r, std::uint8_t g,
                                     std::uint8_t b) {
    outputQueue_.push(OutputCommand::lightbar(deviceId, r, g, b));
}

void SDLGamepadBridge::drainOutputCommands() {
    // drain() takes the batch atomically so the receive thread can keep
    // enqueueing while this executes.
    for (const auto& cmd : outputQueue_.drain()) {
        // Resolved here, on the SDL thread. A controller removed since the
        // command was enqueued is absent from openControllers_, so gc stays null
        // and the command is dropped rather than used after close.
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
            // Return value ignored: -1 just means the pad has no rumble, and
            // neither the caller nor the satellite-side game has any recourse.
            SDL_GameControllerRumble(gc, cmd.strongMagnitude, cmd.weakMagnitude, cmd.durationMs);
        } else {
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
    // A pad claimed by USB-direct streams over raw-HID only, so dropping here is
    // what stops the satellite seeing it twice.
    if (isSuppressed(deviceId)) { return; }

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
    // SDL Y is +down, XUSB is +up.
    st.ly = static_cast<std::int16_t>(-axisValue(gc, SDL_CONTROLLER_AXIS_LEFTY));
    st.rx = axisValue(gc, SDL_CONTROLLER_AXIS_RIGHTX);
    st.ry = static_cast<std::int16_t>(-axisValue(gc, SDL_CONTROLLER_AXIS_RIGHTY));

    processor_->publish(deviceId, st);
}

void SDLGamepadBridge::rebuildJoystickState(int iid) {
    SDL_Joystick* js = nullptr;
    std::string deviceId;
    int vendorId = 0;
    int productId = 0;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        if (auto it = openJoysticks_.find(iid); it != openJoysticks_.end()) { js = it->second; }
        if (auto it = deviceIds_.find(iid); it != deviceIds_.end()) {
            deviceId = it->second.toStdString();
        }
        if (auto it = usbIdentity_.find(iid); it != usbIdentity_.end()) {
            vendorId = it->second.vendorId;
            productId = it->second.productId;
        }
    }
    // A game controller is absent from openJoysticks_, so this no-ops for it.
    if (js == nullptr || deviceId.empty()) { return; }
    if (isSuppressed(deviceId)) { return; }

    // Sized to the device's real counts so the mapper's bounds checks see the
    // true extent.
    const int numAxes = SDL_JoystickNumAxes(js);
    const int numButtons = SDL_JoystickNumButtons(js);
    const int numHats = SDL_JoystickNumHats(js);

    // Fixed caps keep the hot path allocation-free. A pad with more inputs than
    // the cap is truncated, which loses nothing because the layouts reference
    // only low indices.
    constexpr int kMaxAxes = 32;
    constexpr int kMaxButtons = 64;
    constexpr int kMaxHats = 8;
    std::int16_t axes[kMaxAxes] = {0};
    bool buttons[kMaxButtons] = {false};
    std::uint8_t hats[kMaxHats] = {0};

    const int axisCount = numAxes < kMaxAxes ? numAxes : kMaxAxes;
    const int buttonCount = numButtons < kMaxButtons ? numButtons : kMaxButtons;
    const int hatCount = numHats < kMaxHats ? numHats : kMaxHats;
    for (int i = 0; i < axisCount; ++i) { axes[i] = SDL_JoystickGetAxis(js, i); }
    for (int i = 0; i < buttonCount; ++i) { buttons[i] = SDL_JoystickGetButton(js, i) != 0; }
    for (int i = 0; i < hatCount; ++i) { hats[i] = SDL_JoystickGetHat(js, i); }

    JoystickSnapshot snap{};
    snap.axes = axes;
    snap.axisCount = axisCount;
    snap.buttons = buttons;
    snap.buttonCount = buttonCount;
    snap.hats = hats;
    snap.hatCount = hatCount;

    // Copy under remapMtx_ but map OUTSIDE the lock, so a main-thread push never
    // stalls the hot path. A model with no entry maps under the default layout.
    JoystickRemap remap;
    {
        std::lock_guard<std::mutex> lock(remapMtx_);
        if (auto it = joystickRemaps_.find({vendorId, productId}); it != joystickRemaps_.end()) {
            remap = it->second;
        }
    }
    processor_->publish(deviceId, mapJoystick(snap, remap));
}

} // namespace dish::input
