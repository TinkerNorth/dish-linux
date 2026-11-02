// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "Input/PumpWaiter.h"

#include <catch2/catch_test_macros.hpp>

#include <time.h>

#include <chrono>
#include <initializer_list>

using namespace std::chrono_literals;
using dish::input::PumpWaiter;
using dish::input::pump::Clock;
using dish::input::pump::WaitStrategy;

namespace {

std::chrono::nanoseconds monotonicNow() {
    timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return std::chrono::seconds(ts.tv_sec) + std::chrono::nanoseconds(ts.tv_nsec);
}

// Loose enough for a loaded CI runner under a sanitizer; the bench measures the real overshoot.
constexpr auto kPromptly = 50ms;

} // namespace

TEST_CASE("steady_clock reads CLOCK_MONOTONIC, the clock the pump's deadline is absolute on",
          "[pump-waiter]") {
    const auto before = monotonicNow();
    const auto steady = Clock::now().time_since_epoch();
    const auto after = monotonicNow();

    CHECK(steady >= before);
    CHECK(steady <= after);
}

TEST_CASE("a timed wait returns at its deadline, never before it", "[pump-waiter]") {
    const PumpWaiter waiter(WaitStrategy::Timer);
    for (int i = 0; i < 20; ++i) {
        const Clock::time_point deadline = Clock::now() + 500us;
        waiter.waitUntil(deadline);
        const Clock::time_point woke = Clock::now();
        CHECK(woke >= deadline);
        CHECK(woke - deadline < kPromptly);
    }
}

TEST_CASE("a spinning wait returns at its deadline, never before it", "[pump-waiter]") {
    const PumpWaiter waiter(WaitStrategy::Spin);
    const Clock::time_point deadline = Clock::now() + 500us;
    waiter.waitUntil(deadline);
    const Clock::time_point woke = Clock::now();
    CHECK(woke >= deadline);
    CHECK(woke - deadline < kPromptly);
}

TEST_CASE("a deadline already passed returns at once", "[pump-waiter]") {
    for (const WaitStrategy strategy : {WaitStrategy::Timer, WaitStrategy::Spin}) {
        const PumpWaiter waiter(strategy);
        const Clock::time_point start = Clock::now();
        waiter.waitUntil(start - 1ms);
        CHECK(Clock::now() - start < kPromptly);
    }
}
