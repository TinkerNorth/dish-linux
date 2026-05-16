// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#pragma once

#include <QString>

#include <cstdint>
#include <mutex>
#include <utility>
#include <vector>

namespace dish::input {

// A controller output command — a rumble pulse or a lightbar colour — queued
// by the SatelliteClient receive thread for execution on the SDL thread.
//
// Task 1.4 threading fix: SDL_GameControllerRumble / SDL_GameControllerSetLED
// must run on the SDL thread, because that thread is also the only one
// allowed to SDL_GameControllerClose a controller. Resolving the
// SDL_GameController* and calling the SDL function on the receive thread
// races that close → use-after-free. So the receive thread enqueues an
// OutputCommand here and the SDL loop drains it.
enum class OutputKind { Rumble, Lightbar };

struct OutputCommand {
    OutputKind kind = OutputKind::Rumble;
    // The SDL bridge device id the command targets. Resolved to an
    // SDL_GameController* on the SDL thread at drain time.
    QString deviceId;
    // Rumble payload — meaningful when kind == Rumble.
    std::uint16_t strongMagnitude = 0;
    std::uint16_t weakMagnitude = 0;
    std::uint16_t durationMs = 0;
    // Lightbar payload — meaningful when kind == Lightbar.
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;

    // Construct a rumble command.
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

    // Construct a lightbar command.
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
};

// Thread-safe producer/consumer queue for OutputCommands. One producer side
// (the SatelliteClient receive thread, via push) and one consumer side (the
// SDL thread, via drain). No SDL / Qt-event dependency, so it is unit-testable
// on any host without a controller plugged in.
class OutputCommandQueue {
  public:
    // Enqueue a command. Callable from any thread.
    void push(OutputCommand cmd) {
        std::lock_guard<std::mutex> lock(mtx_);
        queue_.push_back(std::move(cmd));
    }

    // Atomically take everything enqueued so far, leaving the queue empty.
    // The caller (SDL thread) then executes the batch outside any lock so a
    // producer can keep enqueueing while the batch runs. Returns the commands
    // in FIFO order.
    std::vector<OutputCommand> drain() {
        std::vector<OutputCommand> batch;
        std::lock_guard<std::mutex> lock(mtx_);
        batch.swap(queue_);
        return batch;
    }

    // Number of queued-but-undrained commands. Test/diagnostic helper.
    std::size_t size() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return queue_.size();
    }

  private:
    mutable std::mutex mtx_;
    std::vector<OutputCommand> queue_;
};

} // namespace dish::input
