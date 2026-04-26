// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#pragma once

#include "GamepadInputProcessor.h"

#include <QObject>
#include <QString>

#include <atomic>
#include <thread>
#include <unordered_map>

// Mirror SDL2's own typedef so we can keep <SDL.h> out of this header.
// The leading underscore is dictated by SDL's struct tag, not our choice.
extern "C" {
// NOLINTNEXTLINE(bugprone-reserved-identifier)
struct _SDL_GameController;
using SDL_GameController = struct _SDL_GameController;
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

    // List of currently-attached devices in (deviceId, displayName) form.
    struct Device {
        QString id;
        QString name;
    };
    QList<Device> devices() const;

  signals:
    void devicesChanged();

  private:
    void runLoop();
    void rebuildState(int iid);

    GamepadInputProcessor* processor_;
    std::thread thread_;
    std::atomic<bool> running_{false};

    // Guarded by mtx_; manipulated only from the input thread except for
    // devices() which reads under lock.
    mutable std::mutex mtx_;
    std::unordered_map<int, SDL_GameController*> openControllers_;
    std::unordered_map<int, QString> deviceIds_;
    std::unordered_map<int, QString> deviceNames_;
};

} // namespace dish::input
