// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "Input/PumpWaiter.h"

#include "core/thread/TrackedMutex.h"

#include <time.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <thread>

namespace dish::input {

namespace {

constexpr std::int64_t kNanosPerSecond = 1'000'000'000;

// The spin-wait hint: x86's pause; elsewhere a yield.
void spinPause() {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#else
    std::this_thread::yield();
#endif
}

// steady_clock reads CLOCK_MONOTONIC on Linux, so its count since the epoch is an absolute time on
// that clock (tests/test_pump_waiter.cpp holds the two clocks together).
timespec monotonicTimespec(pump::Clock::time_point t) {
    const std::int64_t ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();
    timespec ts{};
    ts.tv_sec = static_cast<time_t>(ns / kNanosPerSecond);
    ts.tv_nsec = static_cast<long>(ns % kNanosPerSecond);
    return ts;
}

} // namespace

void PumpWaiter::waitUntil(pump::Clock::time_point deadline) const {
    if (strategy_ == pump::WaitStrategy::Spin) {
        while (pump::Clock::now() < deadline) { spinPause(); }
        return;
    }
    if (deadline <= pump::Clock::now()) { return; }
    thread::blockingRegion("pump.wait");
    const timespec due = monotonicTimespec(deadline);
    while (::clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &due, nullptr) == EINTR) {}
}

} // namespace dish::input
