// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>

namespace dish::input::pump {

using Clock = std::chrono::steady_clock;

enum class WaitStrategy : std::uint8_t { Timer, Spin };

inline constexpr std::chrono::microseconds kDefaultPeriod{500};
inline constexpr std::chrono::microseconds kMinPeriod{125};
inline constexpr std::chrono::microseconds kMaxPeriod{4000};

struct PumpPolicy {
    std::chrono::microseconds period = kDefaultPeriod;
    WaitStrategy wait = WaitStrategy::Timer;

    bool operator==(const PumpPolicy&) const = default;
};

// The settings as read once at start: no period (or a non-positive one) is the default, and one
// outside the range is clamped to it.
constexpr PumpPolicy policyFromSettings(bool spin, std::optional<std::int64_t> periodUs) {
    std::chrono::microseconds period = kDefaultPeriod;
    if (periodUs.has_value() && *periodUs > 0) {
        period = std::clamp(std::chrono::microseconds(*periodUs), kMinPeriod, kMaxPeriod);
    }
    return PumpPolicy{period, spin ? WaitStrategy::Spin : WaitStrategy::Timer};
}

// Cycle n is due at anchor + n * period, whatever happened to cycle n - 1, so the schedule never
// drifts. A cycle that finishes late makes the latest deadline already passed the next one, which
// therefore runs at once; the deadlines before it are skipped rather than run back to back.
class Schedule {
  public:
    Schedule(Clock::time_point anchor, Clock::duration period) : anchor_(anchor), period_(period) {}

    Clock::time_point deadline() const {
        return anchor_ + period_ * static_cast<Clock::rep>(index_);
    }

    std::uint64_t index() const { return index_; }

    void advance(Clock::time_point finished) {
        const std::uint64_t passed =
            finished > anchor_ ? static_cast<std::uint64_t>((finished - anchor_) / period_) : 0;
        index_ = std::max(index_ + 1, passed);
    }

  private:
    Clock::time_point anchor_;
    Clock::duration period_;
    std::uint64_t index_ = 0;
};

} // namespace dish::input::pump
