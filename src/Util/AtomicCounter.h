// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Lock-free monotonically-increasing 64-bit counter (the UDP nonce source on
// the input hot path).

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

    // Returns the value from *before* the increment.
    std::uint64_t next() noexcept { return value_.fetch_add(1, std::memory_order_relaxed); }

    std::uint64_t load() const noexcept { return value_.load(std::memory_order_relaxed); }

    void reset(std::uint64_t to = 0) noexcept { value_.store(to, std::memory_order_relaxed); }

  private:
    std::atomic<std::uint64_t> value_;
};

} // namespace dish::util
