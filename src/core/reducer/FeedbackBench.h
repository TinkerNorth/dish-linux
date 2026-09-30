// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The Diagnostics bench's test buzz: which motors run, how hard and for how
// long, the same strength and time as dish-android's inspector, so a pad feels
// the same test on either client. Rumble is the one feedback surface the bench
// drives, because it is transient by nature: a lightbar colour, the player
// LEDs, a trigger effect and the mute lamp are state the host set, and a test
// would overwrite it with nothing to put it back.

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

} // namespace dish::reducer
