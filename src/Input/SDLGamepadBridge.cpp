// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "SDLGamepadBridge.h"

#include <SDL2/SDL.h>

#include <QLoggingCategory>
#include <QMetaObject>

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
            {
                std::lock_guard<std::mutex> lock(mtx_);
                openControllers_[iid] = gc;
                deviceIds_[iid] = deviceId;
                deviceNames_[iid] = deviceName;
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
                                << "pid=" << QString::number(pid, 16) << "guid=" << guidBuf;
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
        default:
            break;
        }
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
                                   bool hasLightbar, std::uint8_t lightbarR,
                                   std::uint8_t lightbarG, std::uint8_t lightbarB) {
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
