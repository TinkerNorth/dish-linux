// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "Util/AtomicCounter.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <thread>
#include <vector>

using dish::util::AtomicCounter;

TEST_CASE("AtomicCounter starts at the initial value", "[atomic_counter]") {
    AtomicCounter c;
    REQUIRE(c.load() == 0U);

    AtomicCounter d{42};
    REQUIRE(d.load() == 42U);
}

TEST_CASE("AtomicCounter::next returns the previous value and increments", "[atomic_counter]") {
    AtomicCounter c{10};
    REQUIRE(c.next() == 10U);
    REQUIRE(c.next() == 11U);
    REQUIRE(c.load() == 12U);
}

TEST_CASE("AtomicCounter::reset rewinds the counter", "[atomic_counter]") {
    AtomicCounter c;
    (void)c.next();
    (void)c.next();
    c.reset(7);
    REQUIRE(c.load() == 7U);
    REQUIRE(c.next() == 7U);
}

TEST_CASE("AtomicCounter increments are atomic across threads", "[atomic_counter]") {
    constexpr int kThreads = 8;
    constexpr int kPerThread = 25'000;
    AtomicCounter c;

    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i) {
        workers.emplace_back([&] {
            for (int j = 0; j < kPerThread; ++j) { (void)c.next(); }
        });
    }
    for (auto& t : workers) { t.join(); }
    REQUIRE(c.load() == static_cast<std::uint64_t>(kThreads) * kPerThread);
}
