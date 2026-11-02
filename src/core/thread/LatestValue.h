// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#pragma once

#include <atomic>
#include <memory>

namespace dish::thread {

// Hands the newest of a stream of values to ONE consumer thread with no lock on either side. A
// value published before the consumer took the previous one replaces it, and the producer frees
// the one it replaced, which no consumer ever saw. The consumer's check is one relaxed load.
template <typename T> class LatestValue {
  public:
    LatestValue() = default;
    ~LatestValue() { delete pending_.load(std::memory_order_acquire); }

    LatestValue(const LatestValue&) = delete;
    LatestValue& operator=(const LatestValue&) = delete;
    LatestValue(LatestValue&&) = delete;
    LatestValue& operator=(LatestValue&&) = delete;

    void publish(std::unique_ptr<T> next) {
        delete pending_.exchange(next.release(), std::memory_order_acq_rel);
    }

    // Empty unless a value was published since the last take.
    std::unique_ptr<T> take() {
        if (pending_.load(std::memory_order_relaxed) == nullptr) { return {}; }
        return std::unique_ptr<T>(pending_.exchange(nullptr, std::memory_order_acq_rel));
    }

  private:
    std::atomic<T*> pending_{nullptr};
};

} // namespace dish::thread
