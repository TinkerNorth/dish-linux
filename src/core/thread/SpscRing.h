// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace dish::thread {

// A bounded ring for one producer thread and one consumer thread, the satellite's SpscRing: a
// full ring refuses the push, which the producer treats as a drop. Neither side waits or locks.
template <typename T, std::size_t Capacity> class SpscRing {
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0, "a power of two");

  public:
    static constexpr std::size_t kCapacity = Capacity;

    SpscRing() = default;
    SpscRing(const SpscRing&) = delete;
    SpscRing& operator=(const SpscRing&) = delete;
    SpscRing(SpscRing&&) = delete;
    SpscRing& operator=(SpscRing&&) = delete;

    // The producer's cell to fill, or null when the ring is full; publish() makes it visible.
    T* claim() noexcept {
        const std::uint64_t head = head_.value.load(std::memory_order_relaxed);
        if (head - tail_.value.load(std::memory_order_acquire) >= Capacity) { return nullptr; }
        return &cells_[static_cast<std::size_t>(head & kMask)];
    }

    // Sequentially consistent, as the satellite's, so a WorkerWake the producer notifies next sees
    // the push or the consumer sees it on its way to sleep.
    void publish() noexcept {
        head_.value.store(head_.value.load(std::memory_order_relaxed) + 1,
                          std::memory_order_seq_cst);
    }

    bool push(const T& item) noexcept {
        T* cell = claim();
        if (cell == nullptr) { return false; }
        *cell = item;
        publish();
        return true;
    }

    // The consumer's oldest cell, or null when the ring is empty; release() frees it.
    const T* front() const noexcept {
        const std::uint64_t tail = tail_.value.load(std::memory_order_relaxed);
        if (head_.value.load(std::memory_order_seq_cst) == tail) { return nullptr; }
        return &cells_[static_cast<std::size_t>(tail & kMask)];
    }

    void release() noexcept {
        tail_.value.store(tail_.value.load(std::memory_order_relaxed) + 1,
                          std::memory_order_release);
    }

    bool pop(T& out) noexcept {
        const T* cell = front();
        if (cell == nullptr) { return false; }
        out = *cell;
        release();
        return true;
    }

  private:
    static constexpr std::uint64_t kMask = Capacity - 1;

    // Padded to a cache line, so head and tail never share one.
    static constexpr std::size_t kLineBytes = 64;
    struct Position {
        std::atomic<std::uint64_t> value{0};
        std::array<char, kLineBytes - sizeof(std::atomic<std::uint64_t>)> padding{};
    };

    std::array<T, Capacity> cells_{};
    Position head_{};
    Position tail_{};
};

} // namespace dish::thread
