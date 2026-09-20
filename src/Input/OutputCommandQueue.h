// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#pragma once

#include "core/input/UsbOutputReports.h"

#include <QString>

#include <array>
#include <cstdint>
#include <mutex>
#include <utility>
#include <vector>

namespace dish::input {

// A controller output command — a rumble pulse, a lightbar colour, or one of
// the DualSense surfaces SDL has no typed call for (the adaptive triggers, the
// player LEDs, the mic lamp, which ride SDL_GameControllerSendEffect) — queued
// by the SatelliteClient receive thread for execution on the SDL thread.
//
// The SDL thread is the only one allowed to SDL_GameControllerClose, so
// resolving the SDL_GameController* and calling into SDL from the receive
// thread would race that close into a use-after-free. The effect commands
// carry what the game asked for, not bytes: the report is built on the SDL
// thread at drain time, where the per-pad FeedbackState shadow lives.
enum class OutputKind : std::uint8_t { Rumble, Lightbar, TriggerEffects, PlayerLeds, MicLed };

struct OutputCommand {
    OutputKind kind = OutputKind::Rumble;
    // Resolved to an SDL_GameController* on the SDL thread, at drain time.
    QString deviceId;
    // Meaningful when kind == Rumble.
    std::uint16_t strongMagnitude = 0;
    std::uint16_t weakMagnitude = 0;
    std::uint16_t durationMs = 0;
    // Meaningful when kind == Lightbar.
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
    // Meaningful when kind == TriggerEffects: the game's two effect blocks in
    // wire order (left, right), copied through untouched.
    std::array<std::uint8_t, usbout::kTriggerEffectBlockBytes> leftTrigger{};
    std::array<std::uint8_t, usbout::kTriggerEffectBlockBytes> rightTrigger{};
    // Meaningful when kind == PlayerLeds: bit 0 is the leftmost LED.
    std::uint8_t ledMask = 0;
    // Meaningful when kind == MicLed: MSG_MIC_LED's own value (usbout::kMicMuteLed*).
    std::uint8_t micLedState = 0;

    static OutputCommand rumble(QString deviceId, std::uint16_t strong, std::uint16_t weak,
                                std::uint16_t durationMs) {
        OutputCommand c;
        c.kind = OutputKind::Rumble;
        c.deviceId = std::move(deviceId);
        c.strongMagnitude = strong;
        c.weakMagnitude = weak;
        c.durationMs = durationMs;
        return c;
    }

    static OutputCommand lightbar(QString deviceId, std::uint8_t r, std::uint8_t g,
                                  std::uint8_t b) {
        OutputCommand c;
        c.kind = OutputKind::Lightbar;
        c.deviceId = std::move(deviceId);
        c.r = r;
        c.g = g;
        c.b = b;
        return c;
    }

    static OutputCommand
    triggerEffects(QString deviceId,
                   const std::array<std::uint8_t, usbout::kTriggerEffectBlockBytes>& left,
                   const std::array<std::uint8_t, usbout::kTriggerEffectBlockBytes>& right) {
        OutputCommand c;
        c.kind = OutputKind::TriggerEffects;
        c.deviceId = std::move(deviceId);
        c.leftTrigger = left;
        c.rightTrigger = right;
        return c;
    }

    static OutputCommand playerLeds(QString deviceId, std::uint8_t ledMask) {
        OutputCommand c;
        c.kind = OutputKind::PlayerLeds;
        c.deviceId = std::move(deviceId);
        c.ledMask = ledMask;
        return c;
    }

    static OutputCommand micLed(QString deviceId, std::uint8_t state) {
        OutputCommand c;
        c.kind = OutputKind::MicLed;
        c.deviceId = std::move(deviceId);
        c.micLedState = state;
        return c;
    }
};

// Producers: the SatelliteClient receive threads. Consumer: the SDL thread.
// No SDL or Qt dependency, so it is unit-testable without a controller.
class OutputCommandQueue {
  public:
    void push(OutputCommand cmd) {
        std::lock_guard<std::mutex> lock(mtx_);
        queue_.push_back(std::move(cmd));
    }

    // Takes the whole backlog in FIFO order so the SDL thread can run the
    // batch outside the lock while producers keep enqueueing.
    std::vector<OutputCommand> drain() {
        std::vector<OutputCommand> batch;
        std::lock_guard<std::mutex> lock(mtx_);
        batch.swap(queue_);
        return batch;
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return queue_.size();
    }

  private:
    mutable std::mutex mtx_;
    std::vector<OutputCommand> queue_;
};

} // namespace dish::input
