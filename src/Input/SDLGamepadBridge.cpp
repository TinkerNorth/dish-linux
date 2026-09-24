// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "SDLGamepadBridge.h"

#include "core/input/HidTransport.h"
#include "core/input/UsbOutputReports.h"
#include "core/input/UsbReportParsers.h"
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

void SDLGamepadBridge::applySdlHints() {
    // Positional (Xbox-layout) buttons, not label-based, because the USB-direct
    // decoders map by physical position. Without this hint a Switch Pro would
    // disagree with itself across the SDL and Direct paths.
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "0");
    // Qt owns this process's lifecycle. Left to itself SDL installs SIGINT and
    // SIGTERM handlers that raise SDL_QUIT, which nothing here consumes — so
    // the app ignored SIGTERM entirely and a logout sat through systemd's full
    // 90 s wait before the SIGKILL. Packaging CI is what caught it.
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    // A Sony pad on Bluetooth wakes in its simple report mode, where SDL's
    // HIDAPI driver sees buttons and sticks and nothing else: no rumble, no
    // lightbar, no gyro, no touchpad, no effects, and SDL reports none of them
    // as capabilities either. These hints open the pad in enhanced mode
    // instead, so a Bluetooth DualSense or DualShock 4 attaches with the same
    // surfaces as a wired one. The kernel's hid-playstation usually has the
    // pad there already, so this mostly costs nothing; where it does not, the
    // cost SDL documents is that the pad stays in that mode until it is
    // power-cycled.
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5_RUMBLE, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS4_RUMBLE, "1");
}

// False when SDL will not start at all, which leaves the bridge with no devices rather than a
// half-initialised one.
bool SDLGamepadBridge::initSdl() {
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK) != 0) { return false; }
    SDL_GameControllerEventState(SDL_ENABLE);
    // Joystick event delivery is a separate opt-in even though SDL_INIT_JOYSTICK is already up.
    // Needed so generic pads SDL does not recognise as game controllers still surface.
    SDL_JoystickEventState(SDL_ENABLE);
    return true;
}

// What SDL says a freshly opened pad can do. SDL's enable call returns success even for a device
// with no such sensor, so Has and Set must both agree before a sensor is marked capable.
struct SDLGamepadBridge::ControllerCaps {
    bool hasGyro = false;
    bool hasAccel = false;
    bool hasLed = false;
    bool hasTouchpad = false;
    bool hasRumble = false;
    bool bluetooth = false;
    int vendorId = 0;
    int productId = 0;
};

SDLGamepadBridge::ControllerCaps SDLGamepadBridge::probeControllerCaps(SDL_GameController* gc) {
    ControllerCaps caps;
    caps.hasGyro = SDL_GameControllerHasSensor(gc, SDL_SENSOR_GYRO) == SDL_TRUE &&
                   SDL_GameControllerSetSensorEnabled(gc, SDL_SENSOR_GYRO, SDL_TRUE) == 0;
    caps.hasAccel = SDL_GameControllerHasSensor(gc, SDL_SENSOR_ACCEL) == SDL_TRUE &&
                    SDL_GameControllerSetSensorEnabled(gc, SDL_SENSOR_ACCEL, SDL_TRUE) == 0;
    caps.hasLed = SDL_GameControllerHasLED(gc) == SDL_TRUE;
    caps.hasTouchpad = SDL_GameControllerGetNumTouchpads(gc) > 0;
    caps.hasRumble = SDL_GameControllerHasRumble(gc) == SDL_TRUE;
    // SDL returns 0 when it cannot read the descriptor, which the twin-dedup pairing treats as
    // "no identity".
    caps.vendorId = SDL_GameControllerGetVendor(gc);
    caps.productId = SDL_GameControllerGetProduct(gc);
    // SDL's device path is the Win32 HID interface path for HIDAPI and RawInput pads, so the
    // marker check spots a Bluetooth link. A null path (the XInput fallback) reads as
    // not-Bluetooth, which fails safe to the wired presentation.
    const char* devPath = SDL_GameControllerPath(gc);
    caps.bluetooth = devPath != nullptr && dish::input::isBluetoothHidDevicePath(devPath);
    return caps;
}

void SDLGamepadBridge::registerController(int iid, SDL_GameController* gc, const QString& deviceId,
                                          const QString& deviceName, const ControllerCaps& caps) {
    std::lock_guard<std::mutex> lock(mtx_);
    openControllers_[iid] = gc;
    deviceIds_[iid] = deviceId;
    deviceNames_[iid] = deviceName;
    if (caps.hasGyro || caps.hasAccel) { motionCapable_.insert(iid); }
    if (caps.hasLed) { lightbarCapable_.insert(iid); }
    if (caps.hasTouchpad) { touchpadCapable_.insert(iid); }
    if (caps.hasRumble) { rumbleCapable_.insert(iid); }
    if (caps.bluetooth) { bluetoothIids_.insert(iid); }
    usbIdentity_[iid] = {caps.vendorId, caps.productId};
    lastBatteryPoll_[iid] = std::chrono::steady_clock::time_point{};
}

// One-shot capability dump: type, ids and GUID together pin which mapping SDL applied, so a "my
// pad doesn't work" report is diagnosable without a debugger.
void SDLGamepadBridge::logControllerCaps(SDL_GameController* gc, SDL_Joystick* js,
                                         const QString& deviceId, const QString& deviceName,
                                         const ControllerCaps& caps) {
    char guidBuf[64] = {0};
    SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(js), guidBuf, sizeof(guidBuf));
    qCInfo(lcDishInput) << "DEVCAPS id=" << deviceId << "name=" << deviceName
                        << "type=" << static_cast<int>(SDL_GameControllerGetType(gc))
                        << "vid=" << QString::number(caps.vendorId, 16)
                        << "pid=" << QString::number(caps.productId, 16) << "guid=" << guidBuf
                        << "gyro=" << caps.hasGyro << "accel=" << caps.hasAccel
                        << "led=" << caps.hasLed << "rumble=" << caps.hasRumble
                        << "bt=" << caps.bluetooth;
}

void SDLGamepadBridge::onControllerAdded(const SDL_Event& ev) {
    if (isDishVirtualDevice(SDL_GameControllerNameForIndex(ev.cdevice.which))) { return; }
    SDL_GameController* gc = SDL_GameControllerOpen(ev.cdevice.which);
    if (gc == nullptr) { return; }

    SDL_Joystick* js = SDL_GameControllerGetJoystick(gc);
    const int iid = SDL_JoystickInstanceID(js);
    const auto* name = SDL_GameControllerName(gc);
    const QString deviceId = QStringLiteral("sdl:%1").arg(iid);
    const QString deviceName = QString::fromUtf8(name != nullptr ? name : "Gamepad");

    const ControllerCaps caps = probeControllerCaps(gc);
    registerController(iid, gc, deviceId, deviceName, caps);
    logControllerCaps(gc, js, deviceId, deviceName, caps);

    // Pushed from here rather than owned by the processor because the bridge is the only thing
    // that knows when a device shows up.
    processor_->setDeadzones(deviceId.toStdString(), {kDefaultStickFlat, kDefaultTriggerFlat});
    QMetaObject::invokeMethod(this, "devicesChanged", Qt::QueuedConnection);
    rebuildState(iid);
}

void SDLGamepadBridge::onControllerRemoved(const SDL_Event& ev) {
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
    effectState_.erase(iid);
    if (!deviceId.empty()) { processor_->remove(deviceId); }
    QMetaObject::invokeMethod(this, "devicesChanged", Qt::QueuedConnection);
}

void SDLGamepadBridge::onJoystickAdded(const SDL_Event& ev) {
    // Unlike every other event here, this `which` is a device INDEX, not
    // an instance id — it matches SDL_JoystickOpen's argument.
    const int which = ev.jdevice.which;
    // A game controller ALSO emits joystick events, and the controller
    // path owns recognised pads. Opening one here would double it.
    if (SDL_IsGameController(which) == SDL_TRUE) { return; }
    if (isDishVirtualDevice(SDL_JoystickNameForIndex(which))) { return; }
    SDL_Joystick* js = SDL_JoystickOpen(which);
    if (js == nullptr) { return; }
    const int iid = SDL_JoystickInstanceID(js);
    const auto* name = SDL_JoystickName(js);
    const QString deviceId = QStringLiteral("sdl:%1").arg(iid);
    const QString deviceName = QString::fromUtf8(name != nullptr ? name : "Joystick");
    // SDL's joystick API exposes no IMU, LED or controller type, so a raw
    // joystick surfaces as a plain Xbox-kind pad with neither.
    const int vendorId = SDL_JoystickGetVendor(js);
    const int productId = SDL_JoystickGetProduct(js);
    const char* devPath = SDL_JoystickPath(js);
    const bool bluetooth = devPath != nullptr && dish::input::isBluetoothHidDevicePath(devPath);
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
    processor_->setDeadzones(deviceId.toStdString(), {kDefaultStickFlat, kDefaultTriggerFlat});
    QMetaObject::invokeMethod(this, "devicesChanged", Qt::QueuedConnection);
    rebuildJoystickState(iid);
}

void SDLGamepadBridge::onJoystickRemoved(const SDL_Event& ev) {
    const int iid = ev.jdevice.which;
    std::string deviceId;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        // Absent for a controller-path device, so this only acts on raw
        // joysticks we actually opened.
        auto jit = openJoysticks_.find(iid);
        if (jit == openJoysticks_.end()) { return; }
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
}

// One raw-joystick event. The tracked state always rebuilds; the capture page is offered the event
// only when it is listening AND the event was deliberate, which is the gate that keeps idle stick
// jitter and button releases from being read as an assignment. The four call sites differ only in
// what "deliberate" means for their kind.
void SDLGamepadBridge::onRawJoystickInput(int iid, CaptureKind kind, int index, int value,
                                          bool deliberate) {
    rebuildJoystickState(iid);
    if (!deliberate) { return; }
    // Checked second: the flag keeps capture free when off, and it is read per event rather than
    // held, because the page can be left at any point in this loop.
    if (!captureEnabled_.load(std::memory_order_relaxed)) { return; }
    maybeEmitCapture(iid, static_cast<int>(kind), index, value);
}

void SDLGamepadBridge::dispatchSdlEvent(const SDL_Event& ev) {
    switch (ev.type) {
    case SDL_CONTROLLERDEVICEADDED:
        onControllerAdded(ev);
        break;
    case SDL_CONTROLLERDEVICEREMOVED:
        onControllerRemoved(ev);
        break;
    case SDL_JOYDEVICEADDED:
        onJoystickAdded(ev);
        break;
    case SDL_JOYDEVICEREMOVED:
        onJoystickRemoved(ev);
        break;
    case SDL_CONTROLLERAXISMOTION:
    case SDL_CONTROLLERBUTTONDOWN:
    case SDL_CONTROLLERBUTTONUP:
        rebuildState(ev.cdevice.which);
        break;
    case SDL_AUDIODEVICEADDED:
    case SDL_AUDIODEVICEREMOVED:
        // Delivered here because this loop is the process's one SDL event pump; the audio subsystem
        // itself is SdlAudioGateway's (see the signal's comment). A pad's own endpoints appear a
        // beat after its HID interface, so this edge is what re-runs the route matcher.
        QMetaObject::invokeMethod(this, "audioDevicesChanged", Qt::QueuedConnection);
        break;
    case SDL_JOYAXISMOTION:
        // A game controller's joystick events also land here, but its iid is in openControllers_
        // and never openJoysticks_, so this no-ops for it.
        onRawJoystickInput(ev.jaxis.which, CaptureKind::Axis, ev.jaxis.axis, ev.jaxis.value,
                           captureAxisPasses(ev.jaxis.value));
        break;
    case SDL_JOYBUTTONDOWN:
        onRawJoystickInput(ev.jbutton.which, CaptureKind::Button, ev.jbutton.button, 1,
                           captureButtonPasses());
        break;
    case SDL_JOYBUTTONUP:
        // A release is not an assignment, so it rebuilds and offers nothing.
        onRawJoystickInput(ev.jbutton.which, CaptureKind::Button, ev.jbutton.button, 0,
                           /*deliberate=*/false);
        break;
    case SDL_JOYHATMOTION:
        onRawJoystickInput(ev.jhat.which, CaptureKind::Hat, ev.jhat.hat, ev.jhat.value,
                           captureHatPasses(ev.jhat.value));
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
}

// On the SDL thread, where every handle was opened: SDL's own documentation is explicit that a
// device must be closed from the thread that pumps its events.
void SDLGamepadBridge::closeAllDevices() {
    std::lock_guard<std::mutex> lock(mtx_);
    for (auto& [iid, gc] : openControllers_) { SDL_GameControllerClose(gc); }
    openControllers_.clear();
    for (auto& [iid, js] : openJoysticks_) { SDL_JoystickClose(js); }
    openJoysticks_.clear();
    deviceIds_.clear();
    deviceNames_.clear();
}

void SDLGamepadBridge::runLoop() {
    applySdlHints();
    if (!initSdl()) {
        running_.store(false);
        return;
    }

    while (running_.load(std::memory_order_relaxed)) {
        SDL_Event ev;
        if (SDL_WaitEventTimeout(&ev, kSdlWaitMs) == 0) { continue; }
        dispatchSdlEvent(ev);

        // Here, so every SDL_GameController* is resolved and used only on the SDL thread.
        drainOutputCommands();

        // Cheap despite running every iteration: the per-device gate inside collapses it to a 30 s
        // cadence.
        pollBatteries();
    }

    closeAllDevices();
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

// Null unless this event is a GYRO sample from a motion-capable pad that already has an accel to
// pair with. Accel piggy-backs on the next gyro event so the rate limiter sees one stream, not two;
// a gyro-less pad therefore never drives MSG_MOTION, which is fine since it cannot do gyro aim
// anyway.
//
// Deliberately no rotation here: SDL applies the per-model matrix internally for HIDAPI
// controllers, so samples already arrive in the satellite's right-handed frame and only the units
// need converting.
std::optional<SDLGamepadBridge::MotionUpdate>
SDLGamepadBridge::applySensorEvent(const SDL_ControllerSensorEvent& ev) {
    const bool isGyro = ev.sensor == SDL_SENSOR_GYRO;
    const bool isAccel = ev.sensor == SDL_SENSOR_ACCEL;
    if (!isGyro && !isAccel) { return std::nullopt; }

    std::lock_guard<std::mutex> lock(mtx_);
    const int iid = ev.which;
    if (motionCapable_.count(iid) == 0) { return std::nullopt; }

    MotionUpdate update;
    if (auto it = deviceIds_.find(iid); it != deviceIds_.end()) {
        update.deviceId = it->second.toStdString();
    }
    if (update.deviceId.empty()) { return std::nullopt; }

    if (isAccel) {
        lastAccel_[iid] = AccelCache{ev.data[0], ev.data[1], ev.data[2]};
        return std::nullopt;
    }
    // Skip until an accel sample exists: publishing accel{0,0,0} would ship a spurious
    // zero-gravity triple.
    const auto accelIt = lastAccel_.find(iid);
    if (accelIt == lastAccel_.end()) { return std::nullopt; }
    update.accel = accelIt->second;
    return update;
}

void SDLGamepadBridge::handleSensorEvent(const SDL_ControllerSensorEvent& ev) {
    const auto update = applySensorEvent(ev);
    if (!update.has_value()) { return; }
    // Twin-dedup: suppress motion too while USB-direct owns this pad. The accel above is still
    // cached, so the stream resumes on the first gyro sample after the claim is released.
    if (isSuppressed(update->deviceId)) { return; }

    GamepadInputProcessor::MotionSample sample{};
    sample.gyroX = gyroRadPerSecToInt16(ev.data[0]);
    sample.gyroY = gyroRadPerSecToInt16(ev.data[1]);
    sample.gyroZ = gyroRadPerSecToInt16(ev.data[2]);
    sample.accelX = accelMps2ToInt16(update->accel.ax);
    sample.accelY = accelMps2ToInt16(update->accel.ay);
    sample.accelZ = accelMps2ToInt16(update->accel.az);

    processor_->publishMotion(update->deviceId, sample);
}

// `finger` is the 0-based slot; the wire carries only two, so anything higher is dropped.
//
// Only a false-to-true DOWN edge takes a new tracking id, so a MOTION on an already-active finger
// keeps its id. The counter wraps freely: the protocol needs the id to CHANGE on a new contact,
// not to be globally unique.
void SDLGamepadBridge::applyTouchFinger(TouchState& ts, const SDL_ControllerTouchpadEvent& ev) {
    if (ev.finger < 0 || ev.finger >= 2) { return; }
    TouchFinger& f = ts.fingers[ev.finger];
    if (ev.type == SDL_CONTROLLERTOUCHPADUP) {
        f.active = false;
        return;
    }
    if (ev.type == SDL_CONTROLLERTOUCHPADDOWN && !f.active) { ++f.id; }
    f.active = true;
    f.x = touchpadCoordToInt16(ev.x);
    f.y = touchpadCoordToInt16(ev.y);
}

// Null when the event is for a device this bridge does not hold. The controller handle comes back
// with the state so the click can be read live outside the lock.
std::optional<SDLGamepadBridge::TouchUpdate>
SDLGamepadBridge::applyTouchpadEvent(const SDL_ControllerTouchpadEvent& ev) {
    std::lock_guard<std::mutex> lock(mtx_);
    const int iid = ev.which;
    TouchUpdate update;
    if (auto it = deviceIds_.find(iid); it != deviceIds_.end()) {
        update.deviceId = it->second.toStdString();
    }
    if (update.deviceId.empty()) { return std::nullopt; }
    if (auto it = openControllers_.find(iid); it != openControllers_.end()) {
        update.controller = it->second;
    }
    TouchState& ts = touchState_[iid];
    applyTouchFinger(ts, ev);
    update.state = ts;
    return update;
}

void SDLGamepadBridge::handleTouchpadEvent(const SDL_ControllerTouchpadEvent& ev) {
    const auto update = applyTouchpadEvent(ev);
    if (!update.has_value()) { return; }
    // Twin-dedup: the touchpad surface is suppressed too while USB-direct owns the pad. The state
    // above is still tracked, so the surface resumes mid-gesture without a stale finger.
    if (isSuppressed(update->deviceId)) { return; }

    GamepadInputProcessor::TouchpadSample sample{};
    sample.finger0Active = update->state.fingers[0].active;
    sample.finger0Id = update->state.fingers[0].id;
    sample.finger0X = update->state.fingers[0].x;
    sample.finger0Y = update->state.fingers[0].y;
    sample.finger1Active = update->state.fingers[1].active;
    sample.finger1Id = update->state.fingers[1].id;
    sample.finger1X = update->state.fingers[1].x;
    sample.finger1Y = update->state.fingers[1].y;
    // The clickable-pad switch is an ordinary SDL button, so it is read live rather than tracked.
    sample.buttonPressed =
        update->controller != nullptr &&
        SDL_GameControllerGetButton(update->controller, SDL_CONTROLLER_BUTTON_TOUCHPAD) == 1;
    processor_->publishTouchpad(update->deviceId, sample);
}

// Snapshotted under the lock and iterated outside it: holding mtx_ across the publish would block
// applyRumble for no reason. Resolving the joystick handle here is what lets the controller and
// raw-joystick paths share one poll.
std::vector<SDLGamepadBridge::PollEntry> SDLGamepadBridge::batteriesDue(TimePoint now) {
    std::vector<PollEntry> due;
    std::lock_guard<std::mutex> lock(mtx_);
    due.reserve(openControllers_.size() + openJoysticks_.size());
    for (const auto& [iid, gc] : openControllers_) {
        considerForPoll(iid, SDL_GameControllerGetJoystick(gc), now, due);
    }
    for (const auto& [iid, js] : openJoysticks_) { considerForPoll(iid, js, now, due); }
    return due;
}

// Caller holds mtx_. A device polled for the first time is always due, so a pad that has just
// attached shows a charge without waiting out the interval.
void SDLGamepadBridge::considerForPoll(int iid, SDL_Joystick* js, TimePoint now,
                                       std::vector<PollEntry>& due) {
    if (js == nullptr) { return; }
    const auto last = lastBatteryPoll_[iid];
    const bool first = last == TimePoint{};
    if (!first && (now - last) < kBatteryPollInterval) { return; }
    lastBatteryPoll_[iid] = now;

    std::string did;
    if (auto it = deviceIds_.find(iid); it != deviceIds_.end()) { did = it->second.toStdString(); }
    if (did.empty()) { return; }
    due.push_back({iid, std::move(did), js});
}

// Forwarded unconditionally: MSG_BATTERY is a 30 s heartbeat, so the receiver expects a packet
// each interval even when the value is unchanged, and a lost one self-heals on the next tick.
// True when the value moved, which is the only thing the UI needs telling about.
bool SDLGamepadBridge::publishBattery(const PollEntry& e) {
    const auto wire = powerLevelToWire(SDL_JoystickCurrentPowerLevel(e.js));
    processor_->publishBattery(e.deviceId,
                               GamepadInputProcessor::BatterySample{wire.level, wire.status});

    std::lock_guard<std::mutex> lock(mtx_);
    BatterySnapshot& snap = lastBattery_[e.iid];
    if (snap.level == wire.level && snap.status == wire.status) { return false; }
    snap.level = wire.level;
    snap.status = wire.status;
    return true;
}

void SDLGamepadBridge::pollBatteries() {
    bool anyChange = false;
    for (const auto& e : batteriesDue(std::chrono::steady_clock::now())) {
        if (e.js == nullptr) { continue; }
        anyChange = publishBattery(e) || anyChange;
    }
    // One signal per batch, not per device, so the UI rebuilds once.
    if (anyChange) { QMetaObject::invokeMethod(this, "devicesChanged", Qt::QueuedConnection); }
}

void SDLGamepadBridge::applyLightbar(const QString& deviceId, std::uint8_t r, std::uint8_t g,
                                     std::uint8_t b) {
    outputQueue_.push(OutputCommand::lightbar(deviceId, r, g, b));
}

void SDLGamepadBridge::applyTriggerEffects(
    const QString& deviceId, const std::array<std::uint8_t, usbout::kTriggerEffectBlockBytes>& left,
    const std::array<std::uint8_t, usbout::kTriggerEffectBlockBytes>& right) {
    outputQueue_.push(OutputCommand::triggerEffects(deviceId, left, right));
}

void SDLGamepadBridge::applyPlayerLeds(const QString& deviceId, std::uint8_t ledMask) {
    outputQueue_.push(OutputCommand::playerLeds(deviceId, ledMask));
}

void SDLGamepadBridge::applyMicLed(const QString& deviceId, std::uint8_t state) {
    outputQueue_.push(OutputCommand::micLed(deviceId, state));
}

void SDLGamepadBridge::drainOutputCommands() {
    // drain() takes the batch atomically so the receive thread can keep
    // enqueueing while this executes.
    for (const auto& cmd : outputQueue_.drain()) {
        // Resolved here, on the SDL thread. A controller removed since the
        // command was enqueued is absent from openControllers_, so gc stays null
        // and the command is dropped rather than used after close.
        SDL_GameController* gc = nullptr;
        int iid = -1;
        {
            std::lock_guard<std::mutex> lock(mtx_);
            for (const auto& [candidate, did] : deviceIds_) {
                if (did == cmd.deviceId) {
                    if (auto it = openControllers_.find(candidate); it != openControllers_.end()) {
                        gc = it->second;
                        iid = candidate;
                    }
                    break;
                }
            }
        }
        if (gc == nullptr) { continue; }
        switch (cmd.kind) {
        case OutputKind::Rumble:
            // Return value ignored: -1 just means the pad has no rumble, and
            // neither the caller nor the satellite-side game has any recourse.
            SDL_GameControllerRumble(gc, cmd.strongMagnitude, cmd.weakMagnitude, cmd.durationMs);
            break;
        case OutputKind::Lightbar:
            SDL_GameControllerSetLED(gc, cmd.r, cmd.g, cmd.b);
            break;
        case OutputKind::TriggerEffects:
        case OutputKind::PlayerLeds:
        case OutputKind::MicLed:
            sendEffect(gc, iid, cmd);
            break;
        }
    }
}

void SDLGamepadBridge::sendEffect(SDL_GameController* gc, int iid, const OutputCommand& cmd) {
    // The family from the pad's USB identity, which a Bluetooth pad reports
    // too. The builders answer 0 for any family without the surface, and the
    // router upstream only sends a DualSense here, so a 0 is a pad that
    // changed identity under us rather than a wrong report on the wire.
    usbparse::HidParser parser = usbparse::HidParser::None;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        if (auto it = usbIdentity_.find(iid); it != usbIdentity_.end()) {
            parser = usbparse::parserForDevice(it->second.vendorId, it->second.productId);
        }
    }
    usbout::FeedbackState& st = effectState_[iid];
    std::array<std::uint8_t, usbout::kMaxOutputReportBytes> report{};
    std::size_t n = 0;
    switch (cmd.kind) {
    case OutputKind::TriggerEffects:
        n = usbout::buildTriggerEffectsReport(parser, st, cmd.leftTrigger.data(),
                                              cmd.rightTrigger.data(), report.data(),
                                              report.size());
        break;
    case OutputKind::PlayerLeds:
        // The Switch Pro's counter is unused: only the DualSense reaches here.
        n = usbout::buildPlayerLedsReport(parser, st, cmd.ledMask, /*seq=*/0, report.data(),
                                          report.size());
        break;
    case OutputKind::MicLed:
        n = usbout::buildMicMuteLedReport(parser, st, cmd.micLedState, report.data(),
                                          report.size());
        break;
    case OutputKind::Rumble:
    case OutputKind::Lightbar:
        break;
    }
    if (n < usbout::kDs5EffectBodyOffset + usbout::kDs5EffectBodyBytes) { return; }
    // The body without its report id: SDL's driver frames it for the link
    // (see kDs5EffectBodyBytes). Return value ignored for the same reason as
    // rumble's: a refusal is a pad whose driver changed under us, and nothing
    // downstream has a recourse.
    SDL_GameControllerSendEffect(gc, report.data() + usbout::kDs5EffectBodyOffset,
                                 static_cast<int>(usbout::kDs5EffectBodyBytes));
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

// What one raw joystick needs before it can be read at all. js is null for a device SDL opened as
// a game controller instead, which this path does not handle.
struct SDLGamepadBridge::JoystickHandle {
    SDL_Joystick* js = nullptr;
    std::string deviceId;
    int vendorId = 0;
    int productId = 0;
};

SDLGamepadBridge::JoystickHandle SDLGamepadBridge::joystickHandleFor(int iid) {
    JoystickHandle h;
    std::lock_guard<std::mutex> lock(mtx_);
    if (auto it = openJoysticks_.find(iid); it != openJoysticks_.end()) { h.js = it->second; }
    if (auto it = deviceIds_.find(iid); it != deviceIds_.end()) {
        h.deviceId = it->second.toStdString();
    }
    if (auto it = usbIdentity_.find(iid); it != usbIdentity_.end()) {
        h.vendorId = it->second.vendorId;
        h.productId = it->second.productId;
    }
    return h;
}

// Copied under remapMtx_ so a main-thread push never stalls the hot path; the mapping itself runs
// outside the lock. A model with no entry maps under the default layout.
JoystickRemap SDLGamepadBridge::remapFor(int vendorId, int productId) {
    std::lock_guard<std::mutex> lock(remapMtx_);
    if (auto it = joystickRemaps_.find({vendorId, productId}); it != joystickRemaps_.end()) {
        return it->second;
    }
    return JoystickRemap{};
}

void SDLGamepadBridge::rebuildJoystickState(int iid) {
    const JoystickHandle h = joystickHandleFor(iid);
    // A game controller is absent from openJoysticks_, so this no-ops for it.
    if (h.js == nullptr || h.deviceId.empty()) { return; }
    if (isSuppressed(h.deviceId)) { return; }

    // Fixed caps keep the hot path allocation-free, and the buffers live on this frame so the
    // snapshot can borrow rather than own them. A pad with more inputs than a cap is truncated,
    // which loses nothing because the layouts reference only low indices.
    std::int16_t axes[kMaxJoystickAxes] = {0};
    bool buttons[kMaxJoystickButtons] = {false};
    std::uint8_t hats[kMaxJoystickHats] = {0};
    const JoystickSnapshot snap = readJoystick(h.js, axes, buttons, hats);

    processor_->publish(h.deviceId, mapJoystick(snap, remapFor(h.vendorId, h.productId)));
}

// Sized to the device's real counts, so the mapper's bounds checks see the true extent.
JoystickSnapshot SDLGamepadBridge::readJoystick(SDL_Joystick* js,
                                                std::int16_t (&axes)[kMaxJoystickAxes],
                                                bool (&buttons)[kMaxJoystickButtons],
                                                std::uint8_t (&hats)[kMaxJoystickHats]) {
    const int axisCount = std::min(SDL_JoystickNumAxes(js), kMaxJoystickAxes);
    const int buttonCount = std::min(SDL_JoystickNumButtons(js), kMaxJoystickButtons);
    const int hatCount = std::min(SDL_JoystickNumHats(js), kMaxJoystickHats);
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
    return snap;
}

} // namespace dish::input
