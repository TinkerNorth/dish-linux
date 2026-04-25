// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Lock-free monotonically-increasing 64-bit counter. Mirrors the
// AtomicCounter type in dish-mac/Sources/Dish/Util/AtomicCounter.swift.

#pragma once

#include <atomic>
#include <cstdint>

namespace dish::util {

class AtomicCounter {
  public:
    explicit AtomicCounter(std::uint64_t initial = 0) noexcept : value_(initial) {}

    AtomicCounter(const AtomicCounter&) = delete;
    AtomicCounter& operator=(const AtomicCounter&) = delete;
    AtomicCounter(AtomicCounter&&) = delete;
    AtomicCounter& operator=(AtomicCounter&&) = delete;
    ~AtomicCounter() = default;

    // Returns the previous value, then atomically increments. Equivalent to
    // Swift's wrappingIncrementThenLoad on a UInt64 (we never expect to wrap).
    std::uint64_t next() noexcept { return value_.fetch_add(1, std::memory_order_relaxed); }

    std::uint64_t load() const noexcept { return value_.load(std::memory_order_relaxed); }

    void reset(std::uint64_t to = 0) noexcept { value_.store(to, std::memory_order_relaxed); }

  private:
    std::atomic<std::uint64_t> value_;
};

} // namespace dish::util
