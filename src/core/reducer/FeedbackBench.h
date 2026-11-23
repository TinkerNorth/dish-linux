// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The Diagnostics bench (docs/diagnostics-bundle.md, "The bench"): every item
// a pad's path carries, driven from this client alone at the strengths and
// times every client uses, so a pad feels the same test everywhere. Beside
// this file: BenchItems.h names the items and says which a pad is offered,
// BenchSteps.h says what each test writes and when, BenchSurfaces.h keeps the
// state a test borrows (the light bar, the player LEDs, the adaptive triggers,
// the mic lamp) and gives it back to the game, and BenchRuns.h takes one
// answer per run. Here are the two transient items: the body motors' test
// buzz and the trigger motors', which simply stop and borrow nothing.

#pragma once

#include <cstdint>

namespace dish::reducer {

enum class BuzzMotor : std::uint8_t { Weak, Strong, Both };

struct TestBuzz {
    std::uint16_t strong = 0;
    std::uint16_t weak = 0;
    std::uint16_t durationMs = 0;

    bool operator==(const TestBuzz&) const = default;
};

inline constexpr std::uint16_t kTestBuzzMagnitude = 48000;
inline constexpr std::uint16_t kTestBuzzMs = 400;

inline TestBuzz testBuzzFor(BuzzMotor motor) {
    switch (motor) {
    case BuzzMotor::Weak:
        return {0, kTestBuzzMagnitude, kTestBuzzMs};
    case BuzzMotor::Strong:
        return {kTestBuzzMagnitude, 0, kTestBuzzMs};
    case BuzzMotor::Both:
        return {kTestBuzzMagnitude, kTestBuzzMagnitude, kTestBuzzMs};
    }
    return {};
}

enum class TriggerMotor : std::uint8_t { Left, Right, Both };

struct TestTriggerBuzz {
    std::uint16_t left = 0;
    std::uint16_t right = 0;
    std::uint16_t durationMs = 0;

    bool operator==(const TestTriggerBuzz&) const = default;
};

// The trigger motors run at the body motors' strength and time.
inline TestTriggerBuzz testTriggerBuzzFor(TriggerMotor motor) {
    switch (motor) {
    case TriggerMotor::Left:
        return {kTestBuzzMagnitude, 0, kTestBuzzMs};
    case TriggerMotor::Right:
        return {0, kTestBuzzMagnitude, kTestBuzzMs};
    case TriggerMotor::Both:
        return {kTestBuzzMagnitude, kTestBuzzMagnitude, kTestBuzzMs};
    }
    return {};
}

} // namespace dish::reducer
