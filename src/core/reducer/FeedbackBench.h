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
#include <string>
#include <unordered_map>

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

// Each test buzz ends with its own stop, scheduled for when it runs out. A buzz
// started before the last one's stop falls due must not be cut short by that
// stop, so every buzz takes a ticket and its stop is honoured only while the
// ticket is still the pad's latest: dish-android's inspector cancels the
// pending stop to the same end.
class TestBuzzTickets {
  public:
    std::uint64_t issue(const std::string& slotId) { return ++latest_[slotId]; }

    bool isLatest(const std::string& slotId, std::uint64_t ticket) const {
        const auto it = latest_.find(slotId);
        return it != latest_.end() && it->second == ticket;
    }

  private:
    std::unordered_map<std::string, std::uint64_t> latest_;
};

} // namespace dish::reducer
