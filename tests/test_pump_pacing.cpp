// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "core/input/PumpPacing.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <optional>

using namespace std::chrono_literals;
using dish::input::pump::Clock;
using dish::input::pump::kDefaultPeriod;
using dish::input::pump::kMaxPeriod;
using dish::input::pump::kMinPeriod;
using dish::input::pump::policyFromSettings;
using dish::input::pump::PumpPolicy;
using dish::input::pump::Schedule;
using dish::input::pump::WaitStrategy;

namespace {

const Clock::time_point kAnchor = Clock::time_point{} + 10s;

} // namespace

TEST_CASE("the pump defaults to a 500 us timer", "[pump-pacing]") {
    const PumpPolicy policy = policyFromSettings(false, std::nullopt);
    CHECK(policy.period == 500us);
    CHECK(policy.period == kDefaultPeriod);
    CHECK(policy.wait == WaitStrategy::Timer);
}

TEST_CASE("the spin setting picks the spinning wait at the same period", "[pump-pacing]") {
    const PumpPolicy policy = policyFromSettings(true, std::nullopt);
    CHECK(policy.wait == WaitStrategy::Spin);
    CHECK(policy.period == kDefaultPeriod);
}

TEST_CASE("a period from the settings is kept inside the range", "[pump-pacing]") {
    CHECK(policyFromSettings(false, 1000).period == 1000us);
    CHECK(policyFromSettings(false, 10).period == kMinPeriod);
    CHECK(policyFromSettings(false, 100'000).period == kMaxPeriod);
    CHECK(policyFromSettings(false, 0).period == kDefaultPeriod);
    CHECK(policyFromSettings(false, -250).period == kDefaultPeriod);
}

TEST_CASE("a cycle on time waits for the next deadline", "[pump-pacing]") {
    Schedule schedule(kAnchor, 500us);
    CHECK(schedule.deadline() == kAnchor);
    schedule.advance(kAnchor + 120us);
    CHECK(schedule.index() == 1);
    CHECK(schedule.deadline() == kAnchor + 500us);
}

TEST_CASE("a cycle late by less than a period runs the next one at once", "[pump-pacing]") {
    Schedule schedule(kAnchor, 500us);
    const Clock::time_point finished = kAnchor + 700us;
    schedule.advance(finished);
    CHECK(schedule.index() == 1);
    CHECK(schedule.deadline() <= finished);
    schedule.advance(finished + 30us);
    CHECK(schedule.deadline() == kAnchor + 1000us);
}

TEST_CASE("a cycle late by several periods runs once at once and skips the rest", "[pump-pacing]") {
    Schedule schedule(kAnchor, 500us);
    const Clock::time_point finished = kAnchor + 2600us;
    schedule.advance(finished);
    CHECK(schedule.index() == 5);
    CHECK(schedule.deadline() == kAnchor + 2500us);
    schedule.advance(finished + 40us);
    CHECK(schedule.index() == 6);
    CHECK(schedule.deadline() == kAnchor + 3000us);
}

TEST_CASE("jitter inside a period never moves the schedule", "[pump-pacing]") {
    Schedule schedule(kAnchor, 500us);
    for (std::uint64_t cycle = 0; cycle < 10'000; ++cycle) {
        const auto jitter =
            std::chrono::microseconds(static_cast<std::int64_t>((cycle * 37) % 480));
        schedule.advance(schedule.deadline() + jitter);
    }
    CHECK(schedule.index() == 10'000);
    CHECK(schedule.deadline() == kAnchor + 10'000 * 500us);
}

TEST_CASE("a clock reading before the anchor still advances by one", "[pump-pacing]") {
    Schedule schedule(kAnchor, 500us);
    schedule.advance(kAnchor - 1ms);
    CHECK(schedule.index() == 1);
}
