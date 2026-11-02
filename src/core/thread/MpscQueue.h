// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace dish::thread {

// The satellite's bounded multi-producer, single-consumer queue (Vyukov's): each cell carries a
// sequence that says whose turn it is, a producer claims a position with a compare-and-swap, and a
// full queue refuses the push, which the producer counts as a drop. Neither side waits or locks.
template <typename T, std::size_t Capacity> class MpscQueue {
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0, "a power of two");

  public:
    static constexpr std::size_t kCapacity = Capacity;

    MpscQueue() noexcept {
        for (std::size_t i = 0; i < Capacity; ++i) {
            cells_[i].sequence.store(i, std::memory_order_relaxed);
        }
    }
    MpscQueue(const MpscQueue&) = delete;
    MpscQueue& operator=(const MpscQueue&) = delete;
    MpscQueue(MpscQueue&&) = delete;
    MpscQueue& operator=(MpscQueue&&) = delete;
    ~MpscQueue() = default;

    // `fill` writes the claimed cell; false when the queue is full.
    template <typename Fill> bool push(Fill&& fill) noexcept {
        std::uint64_t pos = enqueuePos_.value.load(std::memory_order_relaxed);
        for (;;) {
            Cell& cell = cells_[static_cast<std::size_t>(pos & kMask)];
            const std::uint64_t sequence = cell.sequence.load(std::memory_order_acquire);
            const auto lag = static_cast<std::int64_t>(sequence - pos);
            if (lag == 0) {
                if (enqueuePos_.value.compare_exchange_weak(pos, pos + 1, std::memory_order_seq_cst,
                                                            std::memory_order_relaxed)) {
                    fill(cell.item);
                    cell.sequence.store(pos + 1, std::memory_order_release);
                    return true;
                }
            } else if (lag < 0) {
                dropped_.value.fetch_add(1, std::memory_order_relaxed);
                return false;
            } else {
                pos = enqueuePos_.value.load(std::memory_order_relaxed);
            }
        }
    }

    // The oldest item, copied out before its cell is freed; false when the queue is empty.
    bool pop(T& out) noexcept {
        const std::uint64_t pos = dequeuePos_.value.load(std::memory_order_relaxed);
        Cell& cell = cells_[static_cast<std::size_t>(pos & kMask)];
        if (cell.sequence.load(std::memory_order_acquire) != pos + 1) { return false; }
        out = cell.item;
        cell.sequence.store(pos + Capacity, std::memory_order_release);
        dequeuePos_.value.store(pos + 1, std::memory_order_release);
        return true;
    }

    bool empty() const noexcept {
        const std::uint64_t pos = dequeuePos_.value.load(std::memory_order_relaxed);
        const Cell& cell = cells_[static_cast<std::size_t>(pos & kMask)];
        return cell.sequence.load(std::memory_order_seq_cst) != pos + 1;
    }

    std::uint64_t dropped() const noexcept {
        return dropped_.value.load(std::memory_order_relaxed);
    }

  private:
    static constexpr std::uint64_t kMask = Capacity - 1;
    static constexpr std::size_t kLineBytes = 64;

    struct Cell {
        std::atomic<std::uint64_t> sequence{0};
        T item{};
    };
    struct Position {
        std::atomic<std::uint64_t> value{0};
        std::array<char, kLineBytes - sizeof(std::atomic<std::uint64_t>)> padding{};
    };

    std::array<Cell, Capacity> cells_{};
    Position enqueuePos_{};
    Position dequeuePos_{};
    Position dropped_{};
};

} // namespace dish::thread
