// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#pragma once

#include "core/input/UsbOutputReports.h"
#include "core/thread/Seqlock.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <optional>
#include <thread>

namespace dish::input {

// What a pad's feedback should be now, the newest value of each kind, and whether its controller
// is there as last told.
struct OutputState {
    bool padLinked = false;
    std::uint16_t strong = 0;
    std::uint16_t weak = 0;
    std::uint16_t durationMs = 0;
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
    std::uint8_t ledMask = 0;
    std::uint8_t micLedState = 0;
    std::array<std::uint8_t, usbout::kTriggerEffectBlockBytes> leftTrigger{};
    std::array<std::uint8_t, usbout::kTriggerEffectBlockBytes> rightTrigger{};
};

namespace output {
inline constexpr std::uint32_t kRumble = 1U << 0;
inline constexpr std::uint32_t kLightbar = 1U << 1;
inline constexpr std::uint32_t kTriggerEffects = 1U << 2;
inline constexpr std::uint32_t kPlayerLeds = 1U << 3;
inline constexpr std::uint32_t kMicLed = 1U << 4;
inline constexpr std::uint32_t kPadLink = 1U << 5;
} // namespace output

// One pad's feedback on its way to the thread that drives the pad. A producer (a session's
// receive thread, or the GUI for its own actuation) writes the newest value of a kind inside the
// seqlock and sets the kind's pending bit; the owner takes every pending kind with the newest
// state, so a burst collapses to its last value and nothing queues. Producers take turns on a
// flag held for a few stores; the owner never waits.
class OutputMailbox {
  public:
    struct Taken {
        std::uint32_t kinds = 0;
        OutputState state{};
    };

    void postRumble(std::uint16_t strong, std::uint16_t weak, std::uint16_t durationMs) {
        post(output::kRumble, [&](OutputState& s) {
            s.strong = strong;
            s.weak = weak;
            s.durationMs = durationMs;
        });
    }

    void postLightbar(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
        post(output::kLightbar, [&](OutputState& s) {
            s.r = r;
            s.g = g;
            s.b = b;
        });
    }

    void
    postTriggerEffects(const std::array<std::uint8_t, usbout::kTriggerEffectBlockBytes>& left,
                       const std::array<std::uint8_t, usbout::kTriggerEffectBlockBytes>& right) {
        post(output::kTriggerEffects, [&](OutputState& s) {
            s.leftTrigger = left;
            s.rightTrigger = right;
        });
    }

    void postPlayerLeds(std::uint8_t ledMask) {
        post(output::kPlayerLeds, [&](OutputState& s) { s.ledMask = ledMask; });
    }

    void postMicLed(std::uint8_t state) {
        post(output::kMicLed, [&](OutputState& s) { s.micLedState = state; });
    }

    void postPadLink(bool linked) {
        post(output::kPadLink, [&](OutputState& s) { s.padLinked = linked; });
    }

    bool pending() const { return pending_.load(std::memory_order_relaxed) != 0; }

    // The owner's: forgets what is pending, as for a device that is gone.
    void clear() { pending_.store(0, std::memory_order_relaxed); }

    // The owner's: every kind posted since the last take, with the newest state. Empty when
    // nothing is pending, or when a post is being written, in which case the kinds wait for the
    // next take.
    std::optional<Taken> take() {
        const std::uint32_t kinds = pending_.exchange(0, std::memory_order_acq_rel);
        if (kinds == 0) { return std::nullopt; }
        Taken taken;
        taken.kinds = kinds;
        if (!seq_.read([&] { taken.state = state_.load(); })) {
            pending_.fetch_or(kinds, std::memory_order_relaxed);
            return std::nullopt;
        }
        return taken;
    }

  private:
    template <typename Edit> void post(std::uint32_t kind, Edit&& edit) {
        std::uint32_t free = 0;
        while (!writer_.compare_exchange_weak(free, 1, std::memory_order_acquire,
                                              std::memory_order_relaxed)) {
            free = 0;
            std::this_thread::yield();
        }
        OutputState next = state_.load();
        edit(next);
        seq_.beginWrite();
        state_.store(next);
        seq_.endWrite();
        pending_.fetch_or(kind, std::memory_order_release);
        writer_.store(0, std::memory_order_release);
    }

    std::atomic<std::uint32_t> writer_{0};
    thread::Seqlock seq_;
    thread::AtomicWords<OutputState> state_;
    std::atomic<std::uint32_t> pending_{0};
};

} // namespace dish::input
