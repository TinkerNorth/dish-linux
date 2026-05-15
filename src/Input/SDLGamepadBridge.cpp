// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "SDLGamepadBridge.h"

#include <SDL2/SDL.h>

#include <QLoggingCategory>
#include <QMetaObject>

#include <algorithm>
#include <chrono>
#include <cmath>
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

// Wire scale matching satellite/src/core/types.h. Kept private to this TU
// since they're an implementation detail of the SDL → wire conversion.
//
//   gyro:  SDL gives rad/s; convert to deg/s, then to int16 LSB = 2000/32767
//   accel: SDL gives m/s²;  convert to g (÷ 9.80665), then to int16 LSB = 4/32767
constexpr float kRadPerSecToDegPerSec = 57.295779513f; // 180 / π
constexpr float kGyroInt16PerDegPerSec = 32767.0f / 2000.0f;
constexpr float kAccelInt16PerG = 32767.0f / 4.0f;
constexpr float kGravityMps2 = 9.80665f;

std::int16_t clampInt16(float v) {
    const float c = std::clamp(v, -32768.0f, 32767.0f);
    return static_cast<std::int16_t>(std::lround(c));
}

std::int16_t gyroRadPerSecToInt16(float radPerSec) {
    return clampInt16(radPerSec * kRadPerSecToDegPerSec * kGyroInt16PerDegPerSec);
}

std::int16_t accelMps2ToInt16(float mps2) {
    return clampInt16((mps2 / kGravityMps2) * kAccelInt16PerG);
}

// Map SDL2's joystick power level to the satellite battery wire constants
// + a coarse percent. SDL doesn't expose a continuous level on Windows
// (XInput gamepads only report EMPTY/LOW/MEDIUM/FULL); HID DualSense and
// some 8BitDo pads return UNKNOWN with charging info routed via separate
// hidraw paths. Bucket SDL's coarse enum to (level, status) that the
// satellite + ViGEm DS4 backend understands. Wired pads report 100 % +
// WIRED so Steam Big Picture shows full charge.
struct BatteryReading {
    std::uint8_t level;
    std::uint8_t status;
};
constexpr std::uint8_t kBatteryLevelUnknown = 0xFF;
constexpr std::uint8_t kBatteryStatusUnknown = 0;
constexpr std::uint8_t kBatteryStatusDischarging = 1;
constexpr std::uint8_t kBatteryStatusCharging = 2;
constexpr std::uint8_t kBatteryStatusFull = 3;
constexpr std::uint8_t kBatteryStatusWired = 4;

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
        return {100, kBatteryStatusWired};
    case SDL_JOYSTICK_POWER_UNKNOWN:
    default:
        return {kBatteryLevelUnknown, kBatteryStatusUnknown};
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
    for (const auto& [iid, did] : deviceIds_) { out.append({did, deviceNames_.at(iid)}); }
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
            {
                std::lock_guard<std::mutex> lock(mtx_);
                openControllers_[iid] = gc;
                deviceIds_[iid] = deviceId;
                deviceNames_[iid] = deviceName;
                if (hasGyro || hasAccel) { motionCapable_.insert(iid); }
                lastBatteryPoll_[iid] = std::chrono::steady_clock::time_point{};
            }
            // One-shot device-capability dump — mirrors the SatelliteJNI
            // DEVCAPS log on Android (PR #44/#47). SDL reports the controller
            // type it negotiated (Xbox 360 / DualSense / generic), the vendor
            // / product id, and the GUID; together that pins what mapping was
            // applied so users reporting "my pad doesn't work" get a usable
            // diagnostic without a debugger.
            const auto type = SDL_GameControllerGetType(gc);
            const auto vid = SDL_GameControllerGetVendor(gc);
            const auto pid = SDL_GameControllerGetProduct(gc);
            char guidBuf[64] = {0};
            SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(js), guidBuf, sizeof(guidBuf));
            qCInfo(lcDishInput) << "DEVCAPS id=" << deviceId << "name=" << deviceName
                                << "type=" << static_cast<int>(type)
                                << "vid=" << QString::number(vid, 16)
                                << "pid=" << QString::number(pid, 16) << "guid=" << guidBuf
                                << "gyro=" << hasGyro << "accel=" << hasAccel;
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
                lastBatteryPoll_.erase(iid);
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
        default:
            break;
        }

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
                                   std::uint16_t weakMagnitude, std::uint16_t durationMs,
                                   bool hasLightbar, std::uint8_t lightbarR, std::uint8_t lightbarG,
                                   std::uint8_t lightbarB) {
    SDL_GameController* gc = nullptr;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        for (const auto& [iid, did] : deviceIds_) {
            if (did == deviceId) {
                if (auto it = openControllers_.find(iid); it != openControllers_.end()) {
                    gc = it->second;
                }
                break;
            }
        }
    }
    if (gc == nullptr) { return; }
    // SDL2's `SDL_GameControllerRumble` returns 0 on success, -1 if the device
    // doesn't support rumble — silent: the caller has no recourse beyond the
    // satellite-side game already running, which doesn't know either way.
    SDL_GameControllerRumble(gc, strongMagnitude, weakMagnitude, durationMs);
    if (hasLightbar) {
        // SDL_GameControllerSetLED is a no-op on pads without a lightbar.
        SDL_GameControllerSetLED(gc, lightbarR, lightbarG, lightbarB);
    }
}

void SDLGamepadBridge::handleSensorEvent(const SDL_ControllerSensorEvent& ev) {
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
        if (auto it = lastAccel_.find(iid); it != lastAccel_.end()) { accel = it->second; }
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

    for (const auto& e : due) {
        SDL_Joystick* js = SDL_GameControllerGetJoystick(e.gc);
        if (js == nullptr) { continue; }
        const auto pl = SDL_JoystickCurrentPowerLevel(js);
        const auto wire = powerLevelToWire(pl);
        // The processor coalesces identical (level, status) tuples, so we
        // can call publishBattery unconditionally — a wired pad sitting at
        // 100 % WIRED won't actually emit a packet past the first one.
        GamepadInputProcessor::BatterySample sample{wire.level, wire.status};
        processor_->publishBattery(e.deviceId, sample);
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
