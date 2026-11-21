// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// What came out of each pad, lane by lane, as a count, the last value and
// when (docs/diagnostics-bundle.md, "Every controller path, input and output"),
// and how its speaker and haptics voices stand. The lanes are recorded where
// the router hands a host's feedback to the pad, on the session's receive
// thread, so each field is a relaxed atomic: a sample taken while a write lands
// can pair one write's count with the previous one's value, which a
// diagnostics file can afford and a lock on that thread cannot.

#pragma once

#include "core/reducer/FeedbackRouting.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace dish::diagnostics {

enum class OutputLane : std::uint8_t { Rumble, Lightbar, PlayerLeds, TriggerEffects, MicLed };
inline constexpr std::size_t kOutputLanes = 5;

struct LaneSample {
    std::uint64_t count = 0;
    // The lane's own packing (below); the trigger-effect lane keeps none, its blocks being 22
    // bytes the host wrote verbatim.
    std::uint64_t last = 0;
    // 0 before the lane carried anything.
    std::int64_t lastAtMs = 0;
};

class PadOutputCounters {
  public:
    void record(OutputLane lane, std::uint64_t packedValue, std::int64_t atMs) noexcept {
        Cell& cell = cells_[static_cast<std::size_t>(lane)];
        cell.last.store(packedValue, std::memory_order_relaxed);
        cell.atMs.store(atMs, std::memory_order_relaxed);
        cell.count.fetch_add(1, std::memory_order_relaxed);
    }

    LaneSample sample(OutputLane lane) const noexcept {
        const Cell& cell = cells_[static_cast<std::size_t>(lane)];
        return LaneSample{cell.count.load(std::memory_order_relaxed),
                          cell.last.load(std::memory_order_relaxed),
                          cell.atMs.load(std::memory_order_relaxed)};
    }

  private:
    struct Cell {
        std::atomic<std::uint64_t> count{0};
        std::atomic<std::uint64_t> last{0};
        std::atomic<std::int64_t> atMs{0};
    };
    std::array<Cell, kOutputLanes> cells_{};
};

inline constexpr unsigned kWordBits = 16;
inline constexpr unsigned kByteBits = 8;
inline constexpr std::uint64_t kWordMask = 0xFFFF;
inline constexpr std::uint64_t kByteMask = 0xFF;

inline constexpr std::uint64_t packRumble(std::uint16_t strong, std::uint16_t weak) {
    return (static_cast<std::uint64_t>(strong) << kWordBits) | weak;
}
inline constexpr std::uint16_t rumbleStrongOf(std::uint64_t packed) {
    return static_cast<std::uint16_t>((packed >> kWordBits) & kWordMask);
}
inline constexpr std::uint16_t rumbleWeakOf(std::uint64_t packed) {
    return static_cast<std::uint16_t>(packed & kWordMask);
}

inline constexpr std::uint64_t packRgb(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    return (static_cast<std::uint64_t>(r) << (2 * kByteBits)) |
           (static_cast<std::uint64_t>(g) << kByteBits) | b;
}
inline constexpr std::uint8_t rgbRedOf(std::uint64_t packed) {
    return static_cast<std::uint8_t>((packed >> (2 * kByteBits)) & kByteMask);
}
inline constexpr std::uint8_t rgbGreenOf(std::uint64_t packed) {
    return static_cast<std::uint8_t>((packed >> kByteBits) & kByteMask);
}
inline constexpr std::uint8_t rgbBlueOf(std::uint64_t packed) {
    return static_cast<std::uint8_t>(packed & kByteMask);
}

// Where a lane's output goes: the app's own USB claim, the framework (SDL here), or nowhere.
inline std::string_view feedbackTargetToken(reducer::FeedbackTarget target) {
    switch (target) {
    case reducer::FeedbackTarget::DirectUsb:
        return "directUsb";
    case reducer::FeedbackTarget::Standard:
        return "framework";
    case reducer::FeedbackTarget::None:
        return "none";
    }
    return "none";
}

// A speaker or haptics voice: a refusal that still stands outranks a stream, because a refused
// lane is what took the cap off the descriptor.
enum class VoiceState : std::uint8_t { Open, Closed, Refused };

inline VoiceState voiceStateOf(bool playing, bool refused) {
    if (refused) { return VoiceState::Refused; }
    return playing ? VoiceState::Open : VoiceState::Closed;
}

inline std::string_view voiceStateToken(VoiceState state) {
    switch (state) {
    case VoiceState::Open:
        return "open";
    case VoiceState::Closed:
        return "closed";
    case VoiceState::Refused:
        return "refused";
    }
    return "closed";
}

} // namespace dish::diagnostics
