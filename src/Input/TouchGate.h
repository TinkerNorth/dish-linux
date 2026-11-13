// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#pragma once

#include "Input/GamepadInputProcessor.h"

#include <cstdint>

namespace dish::input {

inline constexpr std::int64_t kTouchpadMoveIntervalNs = 8'000'000;
// Plain UDP can drop a lift frame and leave the receiver holding a phantom finger, so the last
// frame goes again this many times, an interval apart.
inline constexpr int kTouchpadHealResends = 2;

enum class TouchpadSend : std::uint8_t { Skip, Fresh, Heal };

// A heal carries the event time of the frame it repeats: that is how the receiver drops it.
struct TouchpadVerdict {
    TouchpadSend send = TouchpadSend::Skip;
    std::uint32_t eventTimeMs = 0;
};

inline bool isTouchpadEdge(const GamepadInputProcessor::TouchpadSample& last,
                           const GamepadInputProcessor::TouchpadSample& next) {
    const bool contactChanged =
        next.finger0Active != last.finger0Active || next.finger1Active != last.finger1Active;
    const bool clickChanged =
        next.buttonPressed != last.buttonPressed || next.rightPressed != last.rightPressed;
    const bool trackingIdChanged =
        next.finger0Id != last.finger0Id || next.finger1Id != last.finger1Id;
    const bool wheelTurned = next.scrollV != 0;
    return contactChanged || clickChanged || trackingIdChanged || wheelTurned;
}

// Send-on-change for a touchpad that rides every input report: an edge goes at once, a move at
// most once an interval, and an unchanged frame only as a heal.
class TouchGate {
  public:
    TouchpadVerdict decide(const GamepadInputProcessor::TouchpadSample& sample,
                           std::int64_t nowNs) {
        const bool wheelTurned = sample.scrollV != 0;
        if (!wheelTurned && sample.sameTouch(last_)) { return heal(nowNs); }
        return change(sample, nowNs);
    }

  private:
    static constexpr std::int64_t kNsPerMs = 1'000'000;

    TouchpadVerdict change(const GamepadInputProcessor::TouchpadSample& sample,
                           std::int64_t nowNs) {
        const bool insideTheMoveInterval = nowNs - lastSentNs_ < kTouchpadMoveIntervalNs;
        if (!isTouchpadEdge(last_, sample) && insideTheMoveInterval) { return {}; }
        // The wheel is an event, not a level: the latch keeps the levels, so the
        // still frame after a turn is the same touch and heals rather than resends.
        last_ = sample;
        last_.scrollV = 0;
        lastSentNs_ = nowNs;
        eventTimeMs_ = static_cast<std::uint32_t>(nowNs / kNsPerMs);
        healsLeft_ = kTouchpadHealResends;
        return {TouchpadSend::Fresh, eventTimeMs_};
    }

    TouchpadVerdict heal(std::int64_t nowNs) {
        const bool theIntervalElapsed = nowNs - lastSentNs_ >= kTouchpadMoveIntervalNs;
        if (healsLeft_ == 0 || !theIntervalElapsed) { return {}; }
        --healsLeft_;
        lastSentNs_ = nowNs;
        return {TouchpadSend::Heal, eventTimeMs_};
    }

    GamepadInputProcessor::TouchpadSample last_{};
    std::int64_t lastSentNs_ = 0;
    std::uint32_t eventTimeMs_ = 0;
    int healsLeft_ = 0;
};

} // namespace dish::input
