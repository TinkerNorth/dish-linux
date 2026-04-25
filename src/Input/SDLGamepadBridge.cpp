// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "SDLGamepadBridge.h"

#include <SDL2/SDL.h>

#include <QMetaObject>

#include <cstdint>

namespace dish::input {

namespace {

// SDL_GameController axes are int16 [-32768, 32767]; pass through directly.
std::int16_t axisValue(SDL_GameController* gc, SDL_GameControllerAxis axis) {
    return SDL_GameControllerGetAxis(gc, axis);
}

std::uint8_t triggerValue(SDL_GameController* gc, SDL_GameControllerAxis axis) {
    // Triggers are 0..32767 on SDL2; scale to 0..255.
    const int v = SDL_GameControllerGetAxis(gc, axis);
    if (v <= 0) {
        return 0;
    }
    return static_cast<std::uint8_t>((v * 255) / 32767);
}

bool buttonDown(SDL_GameController* gc, SDL_GameControllerButton b) {
    return SDL_GameControllerGetButton(gc, b) != 0;
}

}  // namespace

SDLGamepadBridge::SDLGamepadBridge(GamepadInputProcessor* processor, QObject* parent)
    : QObject(parent), processor_(processor) {}

SDLGamepadBridge::~SDLGamepadBridge() {
    stop();
}

void SDLGamepadBridge::start() {
    if (running_.exchange(true)) {
        return;
    }
    thread_ = std::thread([this] { runLoop(); });
}

void SDLGamepadBridge::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    if (thread_.joinable()) {
        thread_.join();
    }
}

QList<SDLGamepadBridge::Device> SDLGamepadBridge::devices() const {
    std::lock_guard<std::mutex> lock(mtx_);
    QList<Device> out;
    out.reserve(static_cast<int>(deviceIds_.size()));
    for (const auto& [iid, did] : deviceIds_) {
        out.append({did, deviceNames_.at(iid)});
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
        if (SDL_WaitEventTimeout(&ev, 100) == 0) {
            continue;
        }
        switch (ev.type) {
            case SDL_CONTROLLERDEVICEADDED: {
                SDL_GameController* gc = SDL_GameControllerOpen(ev.cdevice.which);
                if (gc == nullptr) {
                    break;
                }
                SDL_Joystick* js = SDL_GameControllerGetJoystick(gc);
                const int iid = SDL_JoystickInstanceID(js);
                const auto* name = SDL_GameControllerName(gc);
                {
                    std::lock_guard<std::mutex> lock(mtx_);
                    openControllers_[iid] = gc;
                    deviceIds_[iid] = QStringLiteral("sdl:%1").arg(iid);
                    deviceNames_[iid] = QString::fromUtf8(name != nullptr ? name : "Gamepad");
                }
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
                if (!deviceId.empty()) {
                    processor_->remove(deviceId);
                }
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
        for (auto& [iid, gc] : openControllers_) {
            SDL_GameControllerClose(gc);
        }
        openControllers_.clear();
        deviceIds_.clear();
        deviceNames_.clear();
    }
    SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK);
}

void SDLGamepadBridge::rebuildState(int iid) {
    SDL_GameController* gc = nullptr;
    std::string deviceId;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        if (auto it = openControllers_.find(iid); it != openControllers_.end()) {
            gc = it->second;
        }
        if (auto it = deviceIds_.find(iid); it != deviceIds_.end()) {
            deviceId = it->second.toStdString();
        }
    }
    if (gc == nullptr || deviceId.empty()) {
        return;
    }

    GamepadInputProcessor::DeviceState st{};
    using B = GamepadInputProcessor::Buttons;
    std::uint16_t btn = 0;
    if (buttonDown(gc, SDL_CONTROLLER_BUTTON_DPAD_UP))    btn |= B::kDpadUp;
    if (buttonDown(gc, SDL_CONTROLLER_BUTTON_DPAD_DOWN))  btn |= B::kDpadDown;
    if (buttonDown(gc, SDL_CONTROLLER_BUTTON_DPAD_LEFT))  btn |= B::kDpadLeft;
    if (buttonDown(gc, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) btn |= B::kDpadRight;
    if (buttonDown(gc, SDL_CONTROLLER_BUTTON_START))      btn |= B::kStart;
    if (buttonDown(gc, SDL_CONTROLLER_BUTTON_BACK))       btn |= B::kBack;
    if (buttonDown(gc, SDL_CONTROLLER_BUTTON_LEFTSTICK))  btn |= B::kLeftThumb;
    if (buttonDown(gc, SDL_CONTROLLER_BUTTON_RIGHTSTICK)) btn |= B::kRightThumb;
    if (buttonDown(gc, SDL_CONTROLLER_BUTTON_LEFTSHOULDER))  btn |= B::kLeftShoulder;
    if (buttonDown(gc, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) btn |= B::kRightShoulder;
    if (buttonDown(gc, SDL_CONTROLLER_BUTTON_A))          btn |= B::kA;
    if (buttonDown(gc, SDL_CONTROLLER_BUTTON_B))          btn |= B::kB;
    if (buttonDown(gc, SDL_CONTROLLER_BUTTON_X))          btn |= B::kX;
    if (buttonDown(gc, SDL_CONTROLLER_BUTTON_Y))          btn |= B::kY;
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

}  // namespace dish::input
